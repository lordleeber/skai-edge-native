#include "skai/metrics.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

TEST(IngestRate, UsesObservedFrameArrivalTimes) {
    skai::IngestRate rate;
    const auto start = std::chrono::steady_clock::time_point{};
    EXPECT_FALSE(rate.observe(start).has_value());
    const auto measured = rate.observe(start + std::chrono::milliseconds(100));
    ASSERT_TRUE(measured.has_value());
    EXPECT_NEAR(*measured, 10.0, 0.01);
    rate.reset();
    EXPECT_FALSE(rate.observe(start + std::chrono::seconds(2)).has_value());
}

TEST(SystemMetricsSampler, ReadsProcessAndFilesystemResources) {
    skai::SystemMetricsSampler sampler(
        std::filesystem::temp_directory_path() / "missing" / "nested");
    const auto first = sampler.sample();
    ASSERT_TRUE(first.memory_rss_bytes.has_value());
    EXPECT_GT(*first.memory_rss_bytes, 0U);
    ASSERT_TRUE(first.disk_free_bytes.has_value());
    EXPECT_GT(*first.disk_free_bytes, 0U);
    EXPECT_FALSE(first.cpu_percent.has_value());
    const auto second = sampler.sample();
    ASSERT_TRUE(second.cpu_percent.has_value());
    EXPECT_GE(*second.cpu_percent, 0.0);
}
