#include "skai/alerts/alert_manager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

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
                                wall, steady).empty());
    EXPECT_TRUE(manager.process(result(2, 1, 0.90f), 100, 100, classes,
                                wall, steady).empty());
    EXPECT_TRUE(manager.process(result(3, 0, 0.69f), 100, 100, classes,
                                wall, steady).empty());
    EXPECT_TRUE(manager.process(result(4, 0, 0.80f), 100, 100, classes,
                                wall, steady).empty());
    const auto alerts = manager.process(result(5, 0, 0.90f), 100, 100, classes,
                                        wall, steady);
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
                                wall, steady).empty());
    const auto alerts = manager.process(result(2, 0, 0.9f, 25.0f), 100, 100,
                                        classes, wall, steady);
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

    auto first = manager.process(result(7, 0, 0.9f), 100, 100, classes, wall, steady);
    EXPECT_TRUE(manager.process(result(8, 0, 0.9f), 100, 100, classes,
                                wall + std::chrono::seconds(5),
                                steady + std::chrono::seconds(5)).empty());
    auto second = manager.process(result(9, 0, 0.9f), 100, 100, classes,
                                  wall + std::chrono::seconds(10),
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
                                wall, steady).empty());
    manager.configure({rule});
    EXPECT_TRUE(manager.process(result(2, 0, 0.9f), 100, 100, classes,
                                wall, steady).empty());
}
