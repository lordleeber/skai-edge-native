#include "loopback_whip_server.hpp"
#include "skai/health.hpp"
#include "skai/webrtc/detection_sei.hpp"
#include "skai/webrtc/ice_runtime_module.hpp"
#include "skai/webrtc/whip_publisher.hpp"

#include <rtc/rtc.hpp>

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace skai_test;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct SourceUnit {
    skai::EncodedAccessUnit unit;
    std::vector<Nal> nals;
};

std::vector<SourceUnit> source_segment(std::uint64_t generation, int frames) {
    std::vector<SourceUnit> units;
    for (auto& encoded : encode_test_stream(frames)) {
        SourceUnit source;
        source.unit.has_pts = true;
        source.unit.pts_ns = encoded.pts_ns;
        source.nals = split_annex_b(encoded.bytes);
        source.unit.keyframe = std::any_of(source.nals.begin(), source.nals.end(),
                                           [](const Nal& nal) { return nal.type == 5; });
        source.unit.discontinuity = units.empty();
        source.unit.source_generation = generation;
        source.unit.bytes = encoded.bytes;
        units.push_back(std::move(source));
    }
    return units;
}

bool same_nals(const std::vector<Nal>& a, const std::vector<Nal>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].type != b[i].type || a[i].ebsp != b[i].ebsp) return false;
    }
    return true;
}

} // namespace

TEST(WhipMetrics, FatalLocationErrorRemainsVisibleAfterWorkerExits) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::Config config;
    config.webrtc.host_interfaces = {"lo"};
    skai::IceRuntimeModule runtime(logger);
    ASSERT_TRUE(runtime.initialize(config)) << runtime.last_error();
    ASSERT_EQ(setenv("WHIP_TOKEN", "loopback-test", 1), 0);
    {
        LoopbackWhipServer server("");
        config.whip.enabled = true;
        config.whip.url = server.url();
        skai::WhipPublisher publisher(logger);
        ASSERT_TRUE(publisher.initialize(config)) << publisher.last_error();
        ASSERT_TRUE(publisher.start()) << publisher.last_error();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline &&
               publisher.metrics().peer_state != "failed") {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto metrics = publisher.metrics();
        EXPECT_EQ(metrics.peer_state, "failed") << logs.str();
        EXPECT_EQ(metrics.ice_state, "closed");
        EXPECT_NE(metrics.last_error.find("Location"), std::string::npos);
        skai::WebRtcDiagnostics lan;
        lan.enabled = true;
        EXPECT_EQ(skai::webrtc_health(lan, metrics).state, skai::HealthState::Failed);
        EXPECT_EQ(server.posts(), 1);
        publisher.stop();
        publisher.wait();
    }
    rtc::Cleanup().wait();
    ASSERT_EQ(unsetenv("WHIP_TOKEN"), 0);
}

TEST(WhipMetrics, RetryDiscardedAccessUnitsAreReportedSeparatelyFromOverflow) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::Config config;
    config.webrtc.host_interfaces = {"lo"};
    skai::IceRuntimeModule runtime(logger);
    ASSERT_TRUE(runtime.initialize(config)) << runtime.last_error();
    ASSERT_EQ(setenv("WHIP_TOKEN", "loopback-test", 1), 0);
    {
        LoopbackWhipServer server("", http::status::service_unavailable);
        config.whip.enabled = true;
        config.whip.url = server.url();
        skai::WhipPublisher publisher(logger);
        ASSERT_TRUE(publisher.initialize(config)) << publisher.last_error();
        skai::EncodedAccessUnit unit;
        for (int index = 0; index < 4; ++index) publisher.publish_access_unit(unit);
        ASSERT_TRUE(publisher.start()) << publisher.last_error();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline &&
               publisher.metrics().media_queue_discarded < 4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto metrics = publisher.metrics();
        EXPECT_EQ(metrics.media_queue_drops, 0U);
        EXPECT_EQ(metrics.media_queue_discarded, 4U) << logs.str();
        publisher.stop();
        publisher.wait();
    }
    rtc::Cleanup().wait();
    ASSERT_EQ(unsetenv("WHIP_TOKEN"), 0);
}

TEST(WhipSei, ViewerRecoversEveryResultAcrossAnRtspRestartAndMediaIsUnchanged) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::Config config;
    config.webrtc.host_interfaces = {"lo"};
    skai::IceRuntimeModule runtime(logger);
    ASSERT_TRUE(runtime.initialize(config)) << runtime.last_error();
    ASSERT_EQ(setenv("WHIP_TOKEN", "loopback-test", 1), 0);

    LoopbackWhipServer server;
    config.whip.enabled = true;
    config.whip.url = server.url();
    skai::WhipPublisher publisher(logger);
    ASSERT_TRUE(publisher.initialize(config)) << publisher.last_error();
    ASSERT_TRUE(publisher.start()) << publisher.last_error();
    ASSERT_TRUE(server.wait_for_open_track(std::chrono::seconds(10))) << logs.str();

    // Two pipeline generations; the second restarts PTS at zero like a reconnect.
    std::vector<SourceUnit> sent = source_segment(1, 40);
    const auto restart_index = sent.size();
    for (auto& unit : source_segment(2, 40)) sent.push_back(std::move(unit));
    ASSERT_EQ(sent.size(), 80U);
    const auto originals = sent;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        publisher.publish_access_unit(sent[i].unit);
        // Result i identifies its source unit through the class name.
        skai::SeiSourceResult result{sent[i].unit.pts_ns, sent[i].unit.source_generation, {}};
        result.boxes.push_back(skai::normalize_sei_box(10, 20, 110, 220, 320, 240,
                                                       "u" + std::to_string(i), 0.9f));
        publisher.publish_detections(std::move(result));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    publisher.stop();
    publisher.wait();
    ASSERT_EQ(unsetenv("WHIP_TOKEN"), 0);

    // The caller's units, i.e. the copies LAN WHEP and recording receive, are untouched.
    for (std::size_t i = 0; i < sent.size(); ++i) {
        ASSERT_EQ(sent[i].unit.bytes, originals[i].unit.bytes) << "unit " << i;
    }
    EXPECT_EQ(server.posts(), 1);
    EXPECT_EQ(server.deletes(), 1);
    const auto frames = server.frames();
    ASSERT_GE(frames.size(), 76U) << logs.str();

    std::vector<std::size_t> source_of_frame;
    std::size_t next_source = 0;
    std::size_t results_seen = 0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        // Removing the one detection SEI must give back the source unit's NALs.
        std::vector<Nal> media;
        std::optional<ParsedSei> sei;
        bool seen_vcl = false;
        for (auto& nal : split_annex_b(frames[f].bytes)) {
            if (nal.type >= 1 && nal.type <= 5) seen_vcl = true;
            auto parsed = parse_user_data_sei(nal);
            if (parsed && parsed->uuid == skai::kDetectionSeiUuid) {
                ASSERT_FALSE(sei) << "frame " << f << " carries two detection SEI";
                EXPECT_FALSE(seen_vcl) << "SEI after a slice in frame " << f;
                EXPECT_TRUE(parsed->trailing_bits_ok);
                sei = std::move(parsed);
                continue;
            }
            media.push_back(std::move(nal));
        }
        while (next_source < sent.size() && !same_nals(media, sent[next_source].nals)) {
            ++next_source;
        }
        ASSERT_LT(next_source, sent.size()) << "frame " << f << " differs from every source unit";
        source_of_frame.push_back(next_source++);
        if (f > 0) {
            const auto step = static_cast<std::int32_t>(frames[f].rtp_timestamp -
                                                         frames[f - 1].rtp_timestamp);
            EXPECT_GT(step, 0) << "RTP timestamp moved backward at frame " << f;
            // Exactly one frame, when the unit sent just before the restart arrived.
            if (source_of_frame[f] == restart_index &&
                source_of_frame[f - 1] == restart_index - 1) {
                EXPECT_EQ(step, 3600);
            }
        }
        if (!sei) continue;

        // Viewer rule: the source frame is the one whose RTP ts equals ts - dt.
        for (const auto& result : parse_results(sei->payload)) {
            ASSERT_EQ(result.boxes.size(), 1U);
            const auto index = std::stoul(result.boxes[0].class_name.substr(1));
            const std::uint32_t source_ts = frames[f].rtp_timestamp - result.dt;
            std::optional<std::size_t> source_frame;
            for (std::size_t k = 0; k <= f; ++k) {
                if (frames[k].rtp_timestamp == source_ts) source_frame = k;
            }
            ASSERT_TRUE(source_frame) << "result u" << index << " points at no frame";
            EXPECT_EQ(source_of_frame[*source_frame], index) << "frame " << f;
            EXPECT_EQ(index >= restart_index, source_of_frame[f] >= restart_index)
                << "result crossed the RTSP restart at frame " << f;
            ++results_seen;
        }
    }
    EXPECT_GE(results_seen, 72U) << logs.str();
    EXPECT_NE(logs.str().find("sei units="), std::string::npos) << logs.str();
    EXPECT_NE(logs.str().find("dt p50="), std::string::npos) << logs.str();
}
