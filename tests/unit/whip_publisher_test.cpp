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
