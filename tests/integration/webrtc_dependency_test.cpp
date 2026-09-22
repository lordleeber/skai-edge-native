#include "skai/webrtc/ice_runtime_module.hpp"

#include <rtc/rtc.hpp>
#include <skai_ice/host_interfaces.hpp>
#include <skai_ice/ice_log.hpp>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct InterfaceAddress {
    std::string name;
    std::string address;
};

InterfaceAddress available_ipv4_interface() {
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return {};
    InterfaceAddress result;
    for (auto* item = addresses; item != nullptr; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
                (item->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        char text[INET_ADDRSTRLEN]{};
        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) {
            result = {item->ifa_name, text};
            break;
        }
    }
    freeifaddrs(addresses);
    return result;
}

} // namespace

TEST(WebRtcDependency, ConfiguresSkaiIceBeforePeerConnectionGathering) {
    const auto interface = available_ipv4_interface();
    ASSERT_FALSE(interface.name.empty());

    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::IceRuntimeModule runtime(logger);
    skai::Config config;
    config.webrtc.host_interfaces = {interface.name};
    config.webrtc.ice_log_verbosity = 2;
    ASSERT_TRUE(runtime.initialize(config));

    EXPECT_EQ(skai_ice::HostInterfaceWhitelist(),
              (std::vector<std::string>{interface.name}));
    EXPECT_EQ(skai_ice::IceLogLevel(), 2);
    EXPECT_NE(logs.str().find("host interfaces=[" + interface.name + "]"),
              std::string::npos);

    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false;
    std::vector<rtc::Candidate> candidates;
    {
        rtc::PeerConnection peer;
        peer.onLocalCandidate([&](rtc::Candidate candidate) {
            std::lock_guard<std::mutex> lock(mutex);
            candidates.push_back(std::move(candidate));
        });
        peer.onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                complete = true;
            }
            changed.notify_all();
        });
        auto channel = peer.createDataChannel("step-23-smoke");
        peer.setLocalDescription();

        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return complete; }));
        ASSERT_FALSE(candidates.empty());
        bool selected_address_found = false;
        for (const auto& candidate : candidates) {
            selected_address_found |= candidate.address() == interface.address;
        }
        EXPECT_TRUE(selected_address_found);
        EXPECT_TRUE(channel != nullptr);
    }
    EXPECT_NE(logs.str().find("module=ice"), std::string::npos);
    EXPECT_NE(logs.str().find("gather: host candidate"), std::string::npos);
    rtc::Cleanup().wait();
}

TEST(WebRtcDependency, RejectsWhitelistWithoutAnyLocalInterface) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::IceRuntimeModule runtime(logger);
    skai::Config config;
    config.webrtc.host_interfaces = {"skai-interface-that-does-not-exist"};

    EXPECT_FALSE(runtime.initialize(config));
    EXPECT_NE(runtime.last_error().find("has no interface present"), std::string::npos);
    EXPECT_NE(runtime.last_error().find("skai-interface-that-does-not-exist"),
              std::string::npos);
    EXPECT_TRUE(logs.str().empty());
}
