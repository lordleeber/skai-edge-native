#include "skai/video/rtsp_recovery.hpp"
#include "skai/video/rtsp_metrics.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
}

TEST(RtspRecovery, BacksOffRepeatedFailuresAndResetsAfterFrame) {
    const Clock::time_point start{};
    skai::RtspRecovery recovery(500ms, 100ms, 400ms);
    recovery.begin(start);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Connecting);
    recovery.fail(start);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Reconnecting);
    EXPECT_FALSE(recovery.retry_due(start + 99ms));
    EXPECT_TRUE(recovery.retry_due(start + 100ms));
    recovery.retry(start + 100ms);
    EXPECT_EQ(recovery.reconnect_count(), 1U);
    recovery.fail(start + 100ms);
    EXPECT_FALSE(recovery.retry_due(start + 299ms));
    EXPECT_TRUE(recovery.retry_due(start + 300ms));
    recovery.retry(start + 300ms);
    recovery.frame(start + 350ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Connected);
    recovery.fail(start + 400ms);
    EXPECT_TRUE(recovery.retry_due(start + 500ms));
}

TEST(RtspRecovery, DetectsPacketLossAndFrameStallWithoutSocketError) {
    const Clock::time_point start{};
    skai::RtspRecovery recovery(500ms, 100ms, 400ms);
    recovery.begin(start);
    recovery.frame(start + 100ms);
    recovery.packet_loss(start + 200ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Degraded);
    recovery.frame(start + 300ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Degraded);
    recovery.frame(start + 1200ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Connected);
    recovery.packet_loss(start + 1300ms);
    EXPECT_FALSE(recovery.check_stall(start + 1699ms));
    EXPECT_TRUE(recovery.check_stall(start + 1700ms));
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Stalled);
    recovery.fail(start + 1700ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Reconnecting);
    recovery.retry(start + 1800ms);
    recovery.frame(start + 1900ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Connected);
}

TEST(RtspMetrics, BackwardsJitterStatDoesNotDoubleCountPacketLoss) {
    skai::MonotonicCounter counter;
    EXPECT_EQ(counter.observe(3), 3U);
    EXPECT_EQ(counter.observe(2), 0U);
    EXPECT_EQ(counter.observe(3), 0U);
    EXPECT_EQ(counter.observe(5), 2U);
    counter.reset();
    EXPECT_EQ(counter.observe(1), 1U);
}
