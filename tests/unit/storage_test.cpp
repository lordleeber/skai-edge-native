#include "skai/storage/alert_repository.hpp"
#include "skai/storage/database.hpp"
#include "skai/web/router.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

TEST(Database, LifecycleReloadsConfigOwnedPathButPreservesExplicitPath) {
    TemporaryDatabase first;
    TemporaryDatabase second;
    skai::Config config;
    config.storage.database_path = first.path();
    skai::Database configured;
    ASSERT_TRUE(configured.initialize(config)) << configured.last_error();
    configured.wait();
    config.storage.database_path = second.path();
    ASSERT_TRUE(configured.initialize(config)) << configured.last_error();
    configured.wait();
    EXPECT_TRUE(std::filesystem::exists(second.path()));
    std::filesystem::remove(second.path());
    skai::Database explicit_path(first.path());
    ASSERT_TRUE(explicit_path.initialize(config)) << explicit_path.last_error();
    explicit_path.wait();
    EXPECT_FALSE(std::filesystem::exists(second.path()));
}

TEST(Database, ApplicationReportsDetailedStorageInitializationFailure) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::Application::Modules modules;
    modules.storage = std::make_unique<skai::Database>("/dev/null/skai-edge.db");
    skai::Application application({}, logger, std::move(modules));
    EXPECT_FALSE(application.initialize());
    EXPECT_EQ(application.last_error_module(), "storage");
    EXPECT_FALSE(application.last_error().empty());
    EXPECT_NE(application.last_error(), "storage.initialize failed");
    EXPECT_NE(application.last_error().find("database"), std::string::npos);
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

TEST(AlertRepository, FiltersAndRemovesOldestAlerts) {
    TemporaryDatabase temporary;
    skai::Database database(temporary.path());
    std::string error;
    ASSERT_TRUE(database.open(error)) << error;
    skai::AlertRepository repository(database);
    auto first = make_alert("first", 1000);
    first.detections = {{0, "person", 0.9, 1, 2, 3, 4}};
    auto second = make_alert("second", 2000);
    second.detections = {{2, "car", 0.8, 5, 6, 7, 8}};
    ASSERT_TRUE(repository.insert(first, error)) << error;
    ASSERT_TRUE(repository.insert(second, error)) << error;

    const auto range = repository.find_by_time_range(0, 2500, 1, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(range.size(), 1U);
    EXPECT_EQ(range[0].id, "second");
    const auto people = repository.find_by_class("person", 10, error);
    ASSERT_EQ(people.size(), 1U);
    EXPECT_EQ(people[0].id, "first");
    const auto excess = repository.find_oldest_excess(1, error);
    ASSERT_EQ(excess.size(), 1U);
    EXPECT_EQ(excess[0].id, "first");
    EXPECT_TRUE(repository.remove("first", error)) << error;
    EXPECT_FALSE(repository.find_by_id("first", error));
}

TEST(HttpRouter, ServesPersistedAlertQueries) {
    TemporaryDatabase temporary;
    skai::Database database(temporary.path());
    std::string error;
    ASSERT_TRUE(database.open(error)) << error;
    skai::AlertRepository repository(database);
    auto alert = make_alert("alert-1", 1700);
    alert.detections[0].class_name = "traffic light";
    ASSERT_TRUE(repository.insert(alert, error)) << error;
    skai::ApiState api;

    const auto list = skai::web::route_request(
        {boost::beast::http::verb::get, "/api/v1/alerts?class=traffic%20light", 11},
        {}, api, &repository);
    EXPECT_EQ(list.result(), boost::beast::http::status::ok);
    EXPECT_NE(list.body().find("\"id\":\"alert-1\""), std::string::npos);
    EXPECT_NE(list.body().find("\"class_name\":\"car\""), std::string::npos);
    const auto item = skai::web::route_request(
        {boost::beast::http::verb::get, "/api/v1/alerts/alert-1", 11},
        {}, api, &repository);
    EXPECT_EQ(item.result(), boost::beast::http::status::ok);
    const auto missing = skai::web::route_request(
        {boost::beast::http::verb::get, "/api/v1/alerts/missing", 11},
        {}, api, &repository);
    EXPECT_EQ(missing.result(), boost::beast::http::status::not_found);
    const auto malformed = skai::web::route_request(
        {boost::beast::http::verb::get, "/api/v1/alerts?class=bad%2", 11},
        {}, api, &repository);
    EXPECT_EQ(malformed.result(), boost::beast::http::status::bad_request);
}
