#include "skai/alerts/alert_manager.hpp"
#include "skai/storage/database.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

const std::vector<std::string> classes = {"person", "car"};

skai::DetectionResult result(std::uint64_t sequence, int class_id,
                             float confidence, float center_x = 50.0f) {
    skai::DetectionResult value;
    value.frame_sequence = sequence;
    value.detections.push_back(
        {class_id, confidence, center_x - 5.0f, 40.0f, center_x + 5.0f, 60.0f});
    return value;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/skai-alert-test-XXXXXX";
        if (const auto* path = mkdtemp(pattern)) path_ = path;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::string child(const std::string& name) const { return path_ + '/' + name; }
private:
    std::string path_;
};

} // namespace

TEST(AlertManager, RequiresClassConfidenceAndConsecutiveFrames) {
    skai::AlertRuleConfig rule;
    rule.class_name = "person";
    rule.confidence = 0.70;
    rule.consecutive_frames = 2;
    rule.cooldown_seconds = 0;
    skai::AlertManager manager;
    manager.configure({rule});
    const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(100)};
    const auto steady = std::chrono::steady_clock::time_point{std::chrono::seconds(100)};

    EXPECT_TRUE(manager.process(result(1, 0, 0.80f), 100, 100, classes,
                                0, wall, steady).empty());
    EXPECT_TRUE(manager.process(result(2, 1, 0.90f), 100, 100, classes,
                                0, wall, steady).empty());
    EXPECT_TRUE(manager.process(result(3, 0, 0.69f), 100, 100, classes,
                                0, wall, steady).empty());
    EXPECT_TRUE(manager.process(result(4, 0, 0.80f), 100, 100, classes,
                                0, wall, steady).empty());
    const auto alerts = manager.process(result(5, 0, 0.90f), 100, 100, classes,
                                        0, wall, steady);
    ASSERT_EQ(alerts.size(), 1U);
    EXPECT_EQ(alerts[0].frame_sequence, 5U);
    ASSERT_EQ(alerts[0].detections.size(), 1U);
    EXPECT_EQ(alerts[0].detections[0].class_name, "person");
}

TEST(AlertManager, FiltersByNormalizedRoiUsingBoxCenter) {
    skai::AlertRuleConfig rule;
    rule.class_name = "person";
    rule.confidence = 0.5;
    rule.consecutive_frames = 1;
    rule.cooldown_seconds = 0;
    rule.roi = skai::AlertRoi{0.0, 0.0, 0.5, 1.0};
    skai::AlertManager manager;
    manager.configure({rule});
    const auto wall = std::chrono::system_clock::now();
    const auto steady = std::chrono::steady_clock::now();

    EXPECT_TRUE(manager.process(result(1, 0, 0.9f, 75.0f), 100, 100, classes,
                                0, wall, steady).empty());
    const auto alerts = manager.process(result(2, 0, 0.9f, 25.0f), 100, 100,
                                        classes, 0, wall, steady);
    EXPECT_EQ(alerts.size(), 1U);
}

TEST(AlertManager, EnforcesCooldownAndPublishesGpsAlertEvent) {
    skai::AlertRuleConfig rule;
    rule.class_name = "person";
    rule.confidence = 0.5;
    rule.consecutive_frames = 1;
    rule.cooldown_seconds = 10;
    auto gps = std::make_shared<skai::GpsState>();
    gps->update(skai::GpsFix{true, "fixed", 25.033964, 121.564468,
                             10.0, 0.9, 12, 10});
    auto events = std::make_shared<skai::EventChannel>();
    std::vector<std::string> published;
    events->subscribe([&](const std::string& event) { published.push_back(event); });
    skai::AlertManager manager(gps, events);
    manager.configure({rule});
    const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(100)};
    const auto steady = std::chrono::steady_clock::time_point{std::chrono::seconds(100)};

    auto first = manager.process(result(7, 0, 0.9f), 100, 100, classes,
                                 0, wall, steady);
    EXPECT_TRUE(manager.process(result(8, 0, 0.9f), 100, 100, classes,
                                0, wall + std::chrono::seconds(5),
                                steady + std::chrono::seconds(5)).empty());
    auto second = manager.process(result(9, 0, 0.9f), 100, 100, classes,
                                  0, wall + std::chrono::seconds(10),
                                  steady + std::chrono::seconds(10));
    ASSERT_EQ(first.size(), 1U);
    ASSERT_EQ(second.size(), 1U);
    ASSERT_TRUE(first[0].gps.has_value());
    EXPECT_DOUBLE_EQ(first[0].gps->longitude, 121.564468);
    ASSERT_EQ(published.size(), 2U);
    EXPECT_NE(published[0].find("\"type\":\"alert\""), std::string::npos);
    EXPECT_NE(published[0].find("\"frame_sequence\":7"), std::string::npos);
    EXPECT_NE(published[0].find("\"gps_source\":\"fixed\""), std::string::npos);
}

TEST(AlertManager, ReconfigureClearsConsecutiveAndCooldownState) {
    skai::AlertRuleConfig rule;
    rule.class_name = "person";
    rule.consecutive_frames = 2;
    rule.cooldown_seconds = 30;
    skai::AlertManager manager;
    const auto wall = std::chrono::system_clock::now();
    const auto steady = std::chrono::steady_clock::now();
    manager.configure({rule});
    EXPECT_TRUE(manager.process(result(1, 0, 0.9f), 100, 100, classes,
                                0, wall, steady).empty());
    manager.configure({rule});
    EXPECT_TRUE(manager.process(result(2, 0, 0.9f), 100, 100, classes,
                                0, wall, steady).empty());
}

TEST(AlertManager, DetectorGenerationChangeClearsPartialStreak) {
    skai::AlertRuleConfig rule;
    rule.class_name = "person";
    rule.consecutive_frames = 2;
    rule.cooldown_seconds = 0;
    skai::AlertManager manager;
    manager.configure({rule});
    const auto wall = std::chrono::system_clock::now();
    const auto steady = std::chrono::steady_clock::now();

    EXPECT_TRUE(manager.process(result(1, 0, 0.9f), 100, 100, classes,
                                10, wall, steady).empty());
    EXPECT_TRUE(manager.process(result(2, 0, 0.9f), 100, 100, classes,
                                12, wall, steady).empty());
    EXPECT_EQ(manager.process(result(3, 0, 0.9f), 100, 100, classes,
                              12, wall, steady).size(), 1U);
}

TEST(AlertManager, PersistsSnapshotMetadataAndCleansUpConsistently) {
    TemporaryDirectory temporary;
    skai::Database database(temporary.child("alerts.db"));
    std::string error;
    ASSERT_TRUE(database.open(error)) << error;
    auto repository = std::make_shared<skai::AlertRepository>(database);
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::AlertManager manager({}, {}, repository, &logger);
    skai::AlertRuleConfig rule;
    rule.class_name = "person";
    rule.consecutive_frames = 1;
    rule.cooldown_seconds = 0;
    manager.configure({rule}, temporary.child("snapshots"), "yolo11s.engine");
    skai::Frame frame;
    frame.width = 4;
    frame.height = 3;
    frame.stride = 12;
    frame.bgr.assign(36, 127);
    auto detections = result(42, 0, 0.9f);
    detections.detections.push_back({0, 0.8f, 1, 1, 3, 2});
    const auto wall = std::chrono::system_clock::time_point{
        std::chrono::seconds(86400)};

    const auto alerts = manager.process(detections, 4, 3, classes, 1, &frame,
                                        wall, std::chrono::steady_clock::now());
    ASSERT_EQ(alerts.size(), 1U);
    EXPECT_TRUE(std::filesystem::is_regular_file(alerts[0].snapshot_path));
    EXPECT_NE(alerts[0].snapshot_path.find("/1970-01-02/"), std::string::npos);
    const auto stored = repository->find_by_id(alerts[0].id, error);
    ASSERT_TRUE(stored) << error;
    EXPECT_EQ(stored->model_version, "yolo11s.engine");
    EXPECT_EQ(stored->detections.size(), 2U);

    const auto snapshot = alerts[0].snapshot_path;
    ASSERT_TRUE(manager.cleanup_oldest(0, error)) << error;
    EXPECT_FALSE(std::filesystem::exists(snapshot));
    EXPECT_FALSE(repository->find_by_id(alerts[0].id, error));

    database.close();
    EXPECT_TRUE(manager.process(result(43, 0, 0.9f), 4, 3, classes, 1, &frame,
                                wall, std::chrono::steady_clock::now()).empty());
    EXPECT_NE(logs.str().find("could not insert alert metadata"), std::string::npos);
    std::size_t jpg_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             temporary.child("snapshots"))) {
        if (entry.path().extension() == ".jpg") ++jpg_count;
    }
    EXPECT_EQ(jpg_count, 0U);
}
