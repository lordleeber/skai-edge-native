#include "skai/storage/alert_repository.hpp"
#include "skai/storage/database.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        char pattern[] = "/tmp/skai-storage-test-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        if (directory) {
            root_ = directory;
            path_ = root_ + "/nested/skai-edge.db";
        }
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::string& path() const { return path_; }

private:
    std::string root_;
    std::string path_;
};

skai::AlertEvent make_alert(std::string id, std::int64_t timestamp_ms) {
    skai::AlertEvent alert;
    alert.id = std::move(id);
    alert.timestamp_ms = timestamp_ms;
    alert.gps = skai::GpsFix{true, "fixed", 25.033964, 121.564468,
                             10.0, 0.9, 12, 10};
    alert.snapshot_path = "/var/lib/skai-edge/alerts/alert.jpg";
    alert.frame_sequence = 42;
    alert.model_version = "yolo11s";
    alert.detections = {
        {0, "person", 0.91, 1.0, 2.0, 30.0, 40.0},
        {2, "car", 0.82, 50.0, 60.0, 90.0, 100.0}};
    return alert;
}

} // namespace

TEST(Database, CreatesParentDirectoryAndAppliesMigrationIdempotently) {
    TemporaryDatabase temporary;
    ASSERT_FALSE(temporary.path().empty());
    std::string error;
    {
        skai::Database database(temporary.path());
        ASSERT_TRUE(database.open(error)) << error;
        EXPECT_EQ(database.schema_version(error), 1);
        EXPECT_TRUE(error.empty());
        EXPECT_TRUE(std::filesystem::exists(temporary.path()));
    }
    skai::Database reopened(temporary.path());
    ASSERT_TRUE(reopened.open(error)) << error;
    EXPECT_EQ(reopened.schema_version(error), 1);
    EXPECT_TRUE(error.empty());
}

TEST(Database, LifecycleClosesAndCanReopenTheDatabase) {
    TemporaryDatabase temporary;
    skai::Config config;
    config.storage.database_path = temporary.path();
    skai::Database database;

    ASSERT_TRUE(database.initialize(config));
    EXPECT_TRUE(database.start());
    EXPECT_TRUE(database.is_open());
    database.stop();
    database.wait();
    EXPECT_FALSE(database.is_open());
    ASSERT_TRUE(database.initialize(config));
    database.wait();
}

TEST(AlertRepository, InsertsAndReadsOneAlertWithDetections) {
    TemporaryDatabase temporary;
    skai::Database database(temporary.path());
    std::string error;
    ASSERT_TRUE(database.open(error)) << error;
    skai::AlertRepository repository(database);
    const auto expected = make_alert("alert-1", 1700000000123);

    ASSERT_TRUE(repository.insert(expected, error)) << error;
    const auto actual = repository.find_by_id(expected.id, error);
    ASSERT_TRUE(actual.has_value()) << error;
    EXPECT_EQ(actual->id, expected.id);
    EXPECT_EQ(actual->timestamp_ms, expected.timestamp_ms);
    ASSERT_TRUE(actual->gps.has_value());
    EXPECT_DOUBLE_EQ(actual->gps->longitude, expected.gps->longitude);
    EXPECT_EQ(actual->gps->source, "fixed");
    EXPECT_EQ(actual->frame_sequence, expected.frame_sequence);
    EXPECT_EQ(actual->model_version, expected.model_version);
    ASSERT_EQ(actual->detections.size(), 2U);
    EXPECT_EQ(actual->detections[1].class_name, "car");
}

TEST(AlertRepository, RollsBackAlertWhenDetectionInsertionFails) {
    TemporaryDatabase temporary;
    skai::Database database(temporary.path());
    std::string error;
    ASSERT_TRUE(database.open(error)) << error;
    skai::AlertRepository repository(database);
    auto alert = make_alert("rollback-me", 1700000000124);
    alert.detections[1].class_name.clear();

    EXPECT_FALSE(repository.insert(alert, error));
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(repository.find_by_id(alert.id, error).has_value());
    EXPECT_TRUE(error.empty());
}

TEST(AlertRepository, SerializesConcurrentReadersAndWriter) {
    TemporaryDatabase temporary;
    skai::Database database(temporary.path());
    std::string error;
    ASSERT_TRUE(database.open(error)) << error;
    skai::AlertRepository repository(database);
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            for (int attempt = 0; attempt < 50; ++attempt) {
                std::string read_error;
                repository.find_recent(10, read_error);
                if (!read_error.empty()) failed = true;
            }
        });
    }
    for (int index = 0; index < 20; ++index) {
        auto alert = make_alert("concurrent-" + std::to_string(index), 1700000000200 + index);
        ASSERT_TRUE(repository.insert(alert, error)) << error;
    }
    for (auto& reader : readers) reader.join();
    EXPECT_FALSE(failed.load());
    const auto latest = repository.find_recent(100, error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(latest.size(), 20U);
    EXPECT_EQ(latest.front().id, "concurrent-19");
}
