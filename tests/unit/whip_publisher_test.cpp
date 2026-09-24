#include "skai/video/encoded_access_unit.hpp"
#include "webrtc/whip_policy.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(WhipPeerState, BriefDisconnectCanRecoverWithoutRecreatingSession) {
    skai::WhipPeerState state;
    const auto start = std::chrono::steady_clock::now();
    state.disconnected(start);
    EXPECT_TRUE(state.is_disconnected());
    EXPECT_FALSE(state.should_reconnect(start + 2s, 10s));
    state.connected();
    EXPECT_FALSE(state.is_disconnected());
    EXPECT_FALSE(state.should_reconnect(start + 20s, 10s));
    state.disconnected(start + 20s);
    EXPECT_TRUE(state.should_reconnect(start + 31s, 10s));
    state.failed();
    EXPECT_TRUE(state.should_reconnect(start + 31s, 10s));
}

TEST(WhipPeerState, StopInterruptsGatheringWait) {
    skai::WhipPeerState state;
    std::atomic<bool> stopping{false};
    std::thread stop_later([&] {
        std::this_thread::sleep_for(20ms);
        stopping = true;
    });
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(state.wait_for_gathering(stopping, 5s));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    stop_later.join();
}

TEST(WhipResource, InvalidCreatedSessionIsFatalToAutomaticRetry) {
    const std::string endpoint = "https://skai-cam.duckdns.org/sfu/cam1/whip";
    EXPECT_THROW(skai::whip_resource_url(endpoint, ""),
                 skai::UnrecoverableWhipError);
    EXPECT_THROW(skai::whip_resource_url(endpoint, "https://other.example/session"),
                 skai::UnrecoverableWhipError);
    EXPECT_EQ(skai::whip_resource_url(endpoint, "/sfu/cam1/whip/abc"),
              "https://skai-cam.duckdns.org/sfu/cam1/whip/abc");
}

namespace {

constexpr std::uint64_t kFrame25 = 40'000'000;

} // namespace

TEST(WhipRtpClock, DerivesTimestampsFromPtsLikeLanWhep) {
    skai::WhipRtpClock clock(12345);
    EXPECT_EQ(clock.timestamp(true, 520'000'000), skai::h264_rtp_timestamp(520'000'000));
    EXPECT_EQ(clock.timestamp(true, 520'000'000 + kFrame25),
              skai::h264_rtp_timestamp(520'000'000) + 3600);
}

TEST(WhipRtpClock, ContinuesForwardAfterRtspReconnectRestartsPts) {
    skai::WhipRtpClock clock(0);
    clock.timestamp(true, 10'000'000'000ULL);
    const auto last = clock.timestamp(true, 10'000'000'000ULL + kFrame25);
    clock.mark_discontinuity();
    const auto resumed = clock.timestamp(true, 520'000'000);
    EXPECT_EQ(resumed, last + 3600);
    EXPECT_EQ(clock.timestamp(true, 520'000'000 + kFrame25), resumed + 3600);
}

TEST(WhipRtpClock, DiscontinuityStaysLatchedUntilAUnitIsSent) {
    skai::WhipRtpClock clock(0);
    clock.timestamp(true, 5'000'000'000ULL);
    const auto last = clock.timestamp(true, 5'000'000'000ULL + kFrame25);
    clock.mark_discontinuity();
    // Keyframe gating skips the flagged unit; the next keyframe is sent later.
    const auto resumed = clock.timestamp(true, 520'000'000 + 3 * kFrame25);
    EXPECT_EQ(resumed, last + 3600);
}

TEST(WhipRtpClock, ContinuesAcrossTheThirtyTwoBitWrap) {
    skai::WhipRtpClock clock(0);
    const std::uint64_t near_wrap = (0xffffffffULL - 1000ULL) * 1'000'000'000ULL / 90'000ULL;
    const auto a = clock.timestamp(true, near_wrap);
    const auto b = clock.timestamp(true, near_wrap + kFrame25);
    clock.mark_discontinuity();
    const auto c = clock.timestamp(true, 0);
    EXPECT_EQ(static_cast<std::uint32_t>(b - a), 3600U);
    EXPECT_EQ(static_cast<std::uint32_t>(c - b), 3600U);
}

TEST(WhipRtpClock, UnitsWithoutPtsStepOneFrame) {
    skai::WhipRtpClock clock(777);
    EXPECT_EQ(clock.timestamp(false, 0), 777U);
    EXPECT_EQ(clock.timestamp(false, 0), 3777U);
}

TEST(WhipRtpClock, BackwardPtsContinuesEvenIfTheFlaggedUnitWasDiscarded) {
    // The discontinuity unit can be dropped by queue overflow or a WHIP pause.
    skai::WhipRtpClock clock(0);
    clock.timestamp(true, 8'000'000'000ULL);
    const auto last = clock.timestamp(true, 8'000'000'000ULL + kFrame25);
    const auto resumed = clock.timestamp(true, 600'000'000);
    EXPECT_EQ(resumed, last + 3600);
    EXPECT_EQ(clock.timestamp(true, 600'000'000 + kFrame25), resumed + 3600);
}
