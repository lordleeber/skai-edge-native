#include "skai/profiling.hpp"
#include "skai/status.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <sstream>

TEST(Profiling, RejectsInvalidSamplesAndPreservesMeasuredZero) {
    skai::TimingSummary summary;
    summary.observe(-1);
    summary.observe(std::numeric_limits<double>::infinity());
    summary.observe(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(summary.count, 0U);
    std::ostringstream unavailable;
    skai::write_timing_json(unavailable, summary);
    EXPECT_NE(unavailable.str().find("\"mean_ms\":null"), std::string::npos);
    summary.observe(0); summary.observe(4);
    EXPECT_EQ(summary.count, 2U);
    EXPECT_DOUBLE_EQ(summary.total_ms, 4);
    EXPECT_DOUBLE_EQ(summary.min_ms, 0);
    EXPECT_DOUBLE_EQ(summary.max_ms, 4);
    std::ostringstream measured;
    skai::write_timing_json(measured, summary);
    EXPECT_NE(measured.str().find("\"mean_ms\":2"), std::string::npos);
}

TEST(Profiling, RuntimeSnapshotKeepsStageTotalsAndResetClearsThem) {
    skai::RuntimeStatus status;
    status.observe_profile(skai::ProfileStage::PreprocessWall, 3);
    status.observe_profile(skai::ProfileStage::PreprocessWall, 5);
    const auto first = status.snapshot();
    const auto stage = static_cast<std::size_t>(skai::ProfileStage::PreprocessWall);
    EXPECT_EQ(first.profiling[stage].count, 2U);
    EXPECT_DOUBLE_EQ(first.profiling[stage].total_ms, 8);
    status.reset_profiling();
    EXPECT_EQ(status.snapshot().profiling[stage].count, 0U);
    EXPECT_EQ(first.profiling[stage].count, 2U);
}
