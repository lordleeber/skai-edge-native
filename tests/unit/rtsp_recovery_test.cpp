#include "skai/video/rtsp_recovery.hpp"

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
    recovery.packet_loss();
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Degraded);
    EXPECT_FALSE(recovery.check_stall(start + 599ms));
    EXPECT_TRUE(recovery.check_stall(start + 600ms));
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Stalled);
    recovery.fail(start + 600ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Reconnecting);
    recovery.retry(start + 700ms);
    recovery.frame(start + 800ms);
    EXPECT_EQ(recovery.health(), skai::SourceHealth::Connected);
}
