#include "skai/video/encoded_access_unit.hpp"
#include "skai/webrtc/whip_publisher.hpp"
#include "webrtc/whip_policy.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>

using namespace std::chrono_literals;

TEST(WhipPublisher, ReportsDisabledUplinkMetrics) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::WhipPublisher publisher(logger);
    skai::Config config;
    config.whip.enabled = false;
    ASSERT_TRUE(publisher.initialize(config));
    const auto metrics = publisher.metrics();
    EXPECT_FALSE(metrics.enabled);
    EXPECT_EQ(metrics.peer_state, "disabled");
    EXPECT_EQ(metrics.ice_state, "disabled");
    EXPECT_EQ(metrics.access_units_sent, 0U);
    EXPECT_EQ(metrics.media_queue_drops, 0U);
}

TEST(WhipPublisher, DisabledReinitializationClearsPreviousQueueCounters) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::WhipPublisher publisher(logger);
    skai::Config config;
    config.whip.enabled = true;
    ASSERT_EQ(setenv("WHIP_TOKEN", "metrics-test", 1), 0);
    ASSERT_TRUE(publisher.initialize(config)) << publisher.last_error();
    skai::EncodedAccessUnit unit;
    for (int index = 0; index < 12; ++index) publisher.publish_access_unit(unit);
    EXPECT_GT(publisher.metrics().media_queue_drops, 0U);
    publisher.stop();
    publisher.wait();

    config.whip.enabled = false;
    ASSERT_TRUE(publisher.initialize(config));
    const auto metrics = publisher.metrics();
    EXPECT_FALSE(metrics.enabled);
    EXPECT_EQ(metrics.media_queue_drops, 0U);
    EXPECT_EQ(metrics.media_queue_discarded, 0U);
    ASSERT_EQ(unsetenv("WHIP_TOKEN"), 0);
}

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
    EXPECT_EQ(clock.timestamp(0, true, 520'000'000), skai::h264_rtp_timestamp(520'000'000));
    EXPECT_EQ(clock.timestamp(0, true, 520'000'000 + kFrame25),
              skai::h264_rtp_timestamp(520'000'000) + 3600);
}

TEST(WhipRtpClock, ContinuesForwardAfterRtspReconnectRestartsPts) {
    skai::WhipRtpClock clock(0);
    clock.timestamp(0, true, 10'000'000'000ULL);
    const auto last = clock.timestamp(0, true, 10'000'000'000ULL + kFrame25);
        const auto resumed = clock.timestamp(1, true, 520'000'000);
    EXPECT_EQ(resumed, last + 3600);
    EXPECT_EQ(clock.timestamp(1, true, 520'000'000 + kFrame25), resumed + 3600);
}

TEST(WhipRtpClock, DiscontinuityStaysLatchedUntilAUnitIsSent) {
    skai::WhipRtpClock clock(0);
    clock.timestamp(0, true, 5'000'000'000ULL);
    const auto last = clock.timestamp(0, true, 5'000'000'000ULL + kFrame25);
        // Keyframe gating skips the flagged unit; the next keyframe is sent later.
    const auto resumed = clock.timestamp(1, true, 520'000'000 + 3 * kFrame25);
    EXPECT_EQ(resumed, last + 3600);
}

TEST(WhipRtpClock, ContinuesAcrossTheThirtyTwoBitWrap) {
    skai::WhipRtpClock clock(0);
    const std::uint64_t near_wrap = (0xffffffffULL - 1000ULL) * 1'000'000'000ULL / 90'000ULL;
    const auto a = clock.timestamp(0, true, near_wrap);
    const auto b = clock.timestamp(0, true, near_wrap + kFrame25);
        const auto c = clock.timestamp(1, true, 0);
    EXPECT_EQ(static_cast<std::uint32_t>(b - a), 3600U);
    EXPECT_EQ(static_cast<std::uint32_t>(c - b), 3600U);
}

TEST(WhipRtpClock, UnitsWithoutPtsStepOneFrame) {
    skai::WhipRtpClock clock(777);
    EXPECT_EQ(clock.timestamp(0, false, 0), 777U);
    EXPECT_EQ(clock.timestamp(0, false, 0), 3777U);
}

TEST(WhipRtpClock, NewRestartGenerationContinuesAfterShortSessionWithoutFlag) {
    // The flagged unit may be discarded by queue overflow or a WHIP pause, and
    // a short session leaves only a small backward PTS step; the source
    // generation carried by every unit still identifies the restart.
    skai::WhipRtpClock clock(0);
    clock.timestamp(4, true, 520'000'000);
    const auto last = clock.timestamp(4, true, 520'000'000 + kFrame25);
    // The new pipeline's first sent keyframe lies a few frames ahead of the old PTS.
    const auto resumed = clock.timestamp(5, true, 520'000'000 + 4 * kFrame25);
    EXPECT_EQ(resumed, last + 3600);
    const auto small_backward = clock.timestamp(6, true, 530'000'000);
    EXPECT_EQ(small_backward, resumed + 3600);
}

TEST(WhipRtpClock, NonIncreasingPtsWithinOneGenerationDoesNotDriftAhead) {
    skai::WhipRtpClock clock(0);
    const std::uint64_t base = 2'000'000'000ULL;
    const auto first = clock.timestamp(0, true, base);
    clock.timestamp(0, true, base + kFrame25);
    clock.timestamp(0, true, base + kFrame25);          // duplicate PTS
    clock.timestamp(0, true, base + kFrame25 - 1'000'000); // small reorder
    for (int i = 2; i <= 50; ++i) {
        EXPECT_EQ(clock.timestamp(0, true, base + i * kFrame25),
                  first + static_cast<std::uint32_t>(i) * 3600);
    }
}

TEST(WhipRtpClock, RestartAfterOnlyOnePtsUnitStepsADefaultFrame) {
    skai::WhipRtpClock clock(0);
    const auto first = clock.timestamp(0, true, 9'000'000'000ULL);
    EXPECT_EQ(clock.timestamp(1, true, 520'000'000), first + 3000);
}

TEST(WhipRtpClock, SwitchingFromMissingPtsToPtsKeepsMovingForward) {
    skai::WhipRtpClock clock(4'000'000'000U);
    const auto a = clock.timestamp(0, false, 0);
    const auto b = clock.timestamp(0, false, 0);
    const auto c = clock.timestamp(0, true, 520'000'000);
    EXPECT_EQ(b, a + 3000);
    EXPECT_EQ(c, b + 3000);
    EXPECT_EQ(clock.timestamp(0, true, 520'000'000 + kFrame25), c + 3600);
}
