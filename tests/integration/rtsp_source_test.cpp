#include "rtsp_test_server.hpp"
#include "skai/video/encoded_access_unit.hpp"
#include "skai/video/rtsp_source.hpp"
#include "skai/video/rtsp_video_module.hpp"

#include <gtest/gtest.h>

#include <optional>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool plugin_available(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

bool wait_for_health(skai::RtspSource& source, skai::SourceHealth expected,
                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (source.diagnostics().health == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return source.diagnostics().health == expected;
}

std::size_t open_file_descriptors() {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator("/proc/self/fd"),
        std::filesystem::directory_iterator{}));
}

class HangingRtspEndpoint {
public:
    HangingRtspEndpoint() {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 1) != 0) return;
        socklen_t length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) return;
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] {
            accepted_ = accept(listener_, nullptr, nullptr);
        });
    }
    ~HangingRtspEndpoint() {
        if (listener_ >= 0) {
            shutdown(listener_, SHUT_RDWR);
            close(listener_);
        }
        if (worker_.joinable()) worker_.join();
        if (accepted_ >= 0) close(accepted_);
    }
    int port() const { return port_; }
    bool accepted() const { return accepted_ >= 0; }
private:
    int listener_ = -1;
    int port_ = 0;
    std::atomic<int> accepted_{-1};
    std::thread worker_;
};

void receives_decoded_frames(skai::test::RtspTestServer::Codec codec,
                             const std::string& expected_codec) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server(codec);
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.transport = "tcp";
    config.latency_ms = 50;
    EXPECT_FALSE(source.diagnostics().url_configured);
    ASSERT_TRUE(source.start(config, error)) << error << output.str();

    auto first = frames.pop_for(std::chrono::seconds(5));
    ASSERT_TRUE(first.has_value()) << source.diagnostics().last_error << output.str();
    auto second = frames.pop_for(std::chrono::seconds(5));
    ASSERT_TRUE(second.has_value()) << source.diagnostics().last_error << output.str();
    EXPECT_GT(second->sequence, first->sequence);
    EXPECT_GE(second->timestamp, first->timestamp);
    EXPECT_EQ(first->width, 160);
    EXPECT_EQ(first->height, 120);
    EXPECT_EQ(first->stride, 160 * 3);
    EXPECT_EQ(first->bgr.size(), 160U * 120U * 3U);

    const auto health = source.diagnostics();
    EXPECT_EQ(health.health, skai::SourceHealth::Connected);
    EXPECT_EQ(health.codec, expected_codec);
    EXPECT_EQ(health.width, 160);
    EXPECT_EQ(health.height, 120);
    EXPECT_EQ(health.fps_num, 10);
    EXPECT_EQ(health.fps_den, 1);
    EXPECT_GE(health.frames_received, 2U);
    EXPECT_TRUE(health.url_configured);
    EXPECT_EQ(health.transport, "tcp");
    EXPECT_GT(health.fps_in, 0.0);
    EXPECT_GE(health.last_frame_age_ms, 0);
    const auto metrics = skai::serialize_rtsp_metrics(health);
    EXPECT_NE(metrics.find("\"connected\":true"), std::string::npos);
    EXPECT_NE(metrics.find("\"frames_dropped\""), std::string::npos);
    EXPECT_NE(metrics.find("\"frames_discarded\""), std::string::npos);
    EXPECT_EQ(metrics.find("secret"), std::string::npos);
    EXPECT_LE(frames.size(), 2U);
    source.stop();
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Stopped);
}

} // namespace

TEST(RtspSource, DecodesH264IntoBoundedInferenceQueue) {
    receives_decoded_frames(skai::test::RtspTestServer::Codec::H264, "H264");
}

TEST(RtspSource, PublishesSourceH264AccessUnitsWithoutReencoding) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> access_units(30);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software, true, {},
                            &access_units);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.transport = "tcp";
    config.latency_ms = 50;
    ASSERT_TRUE(source.start(config, error)) << error << output.str();

    ASSERT_TRUE(frames.pop_for(std::chrono::seconds(3)).has_value()) << output.str();
    bool saw_keyframe = false;
    bool saw_discontinuity = false;
    bool saw_pts = false;
    std::uint64_t previous_sequence = 0;
    for (int attempt = 0; attempt < 30 && !saw_keyframe; ++attempt) {
        auto unit = access_units.pop_for(std::chrono::milliseconds(200));
        if (!unit) continue;
        EXPECT_GT(unit->sequence, previous_sequence);
        previous_sequence = unit->sequence;
        ASSERT_GE(unit->bytes.size(), 4U);
        EXPECT_EQ(unit->bytes[0], 0U);
        EXPECT_EQ(unit->bytes[1], 0U);
        EXPECT_TRUE((unit->bytes[2] == 1U) ||
                    (unit->bytes[2] == 0U && unit->bytes[3] == 1U));
        saw_discontinuity = saw_discontinuity || unit->discontinuity;
        saw_pts = saw_pts || unit->has_pts;
        saw_keyframe = saw_keyframe || unit->keyframe;
    }
    EXPECT_TRUE(saw_discontinuity);
    EXPECT_TRUE(saw_keyframe);
    EXPECT_TRUE(saw_pts);
    source.stop();
}

TEST(RtspSource, StampsFramesAndAccessUnitsWithThePipelineGeneration) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> access_units(8);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software, true, {},
                            &access_units);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.transport = "tcp";
    config.latency_ms = 50;
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    const auto first_frame = frames.pop_for(std::chrono::seconds(3));
    const auto first_unit = access_units.pop_for(std::chrono::seconds(3));
    ASSERT_TRUE(first_frame && first_unit) << output.str();
    EXPECT_GT(first_frame->source_generation, 0U);
    EXPECT_EQ(first_unit->source_generation, first_frame->source_generation);

    server.stop();
    ASSERT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
    ASSERT_TRUE(server.start(error)) << error;
    std::optional<skai::Frame> frame;
    std::optional<skai::EncodedAccessUnit> unit;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           (!frame || frame->source_generation == first_frame->source_generation ||
            !unit || unit->source_generation == first_unit->source_generation)) {
        if (auto next = frames.pop_for(std::chrono::milliseconds(50))) frame = std::move(next);
        if (auto next = access_units.pop_for(std::chrono::milliseconds(50))) unit = std::move(next);
    }
    source.stop();
    ASSERT_TRUE(frame && unit) << output.str();
    EXPECT_GT(frame->source_generation, first_frame->source_generation) << output.str();
    EXPECT_EQ(unit->source_generation, frame->source_generation);
}

TEST(RtspSource, RejectsHighProfileH264PassthroughButStillDecodesForInference) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server(skai::test::RtspTestServer::Codec::H264High);
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> access_units(30);
    std::mutex status_mutex;
    std::condition_variable status_changed;
    bool incompatible = false;
    std::string reason;
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software, true, {},
                            &access_units, {}, [&](bool available, const std::string& value) {
        std::lock_guard<std::mutex> lock(status_mutex);
        if (!available && value.find("constrained-baseline") != std::string::npos) {
            incompatible = true;
            reason = value;
            status_changed.notify_all();
        }
    });
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.transport = "tcp";
    config.latency_ms = 50;
    ASSERT_TRUE(source.start(config, error)) << error << output.str();

    ASSERT_TRUE(frames.pop_for(std::chrono::seconds(5)).has_value()) << output.str();
    std::unique_lock<std::mutex> lock(status_mutex);
    ASSERT_TRUE(status_changed.wait_for(lock, std::chrono::seconds(2),
                                        [&] { return incompatible; })) << output.str();
    EXPECT_NE(reason.find("constrained-baseline"), std::string::npos);
    lock.unlock();
    EXPECT_FALSE(access_units.pop_for(std::chrono::milliseconds(100)).has_value());
    source.stop();
}

TEST(RtspSource, DecodesH265IntoBoundedInferenceQueue) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    if (!plugin_available("x265enc") || !plugin_available("avdec_h265")) {
        GTEST_SKIP() << "H.265 test plugins unavailable";
    }
    receives_decoded_frames(skai::test::RtspTestServer::Codec::H265, "H265");
}

TEST(RtspSource, ReportsRecordingAndWebrtcUnavailableForH265) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    if (!plugin_available("x265enc") || !plugin_available("avdec_h265")) {
        GTEST_SKIP() << "H.265 test plugins unavailable";
    }
    skai::test::RtspTestServer server(skai::test::RtspTestServer::Codec::H265);
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> access_units(4);
    std::mutex status_mutex;
    std::condition_variable status_changed;
    bool unsupported = false;
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software, true, {},
                            &access_units, {}, [&](bool available, const std::string& reason) {
        std::lock_guard<std::mutex> lock(status_mutex);
        if (!available && reason.find("require H.264") != std::string::npos) {
            unsupported = true;
            status_changed.notify_all();
        }
    });
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.transport = "tcp";
    config.latency_ms = 50;
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    ASSERT_TRUE(frames.pop_for(std::chrono::seconds(5)).has_value()) << output.str();
    std::unique_lock<std::mutex> lock(status_mutex);
    EXPECT_TRUE(status_changed.wait_for(lock, std::chrono::seconds(2),
                                        [&] { return unsupported; })) << output.str();
    lock.unlock();
    EXPECT_FALSE(access_units.pop_for(std::chrono::milliseconds(100)).has_value());
    source.stop();
}

TEST(RtspSource, ReceivesH264OverUdp) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.transport = "udp";
    config.latency_ms = 50;
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    EXPECT_TRUE(frames.pop_for(std::chrono::seconds(3)).has_value());
}

TEST(RtspSource, RejectsInvalidUrlBeforePipelineStartup) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger);
    skai::VideoConfig config;
    config.rtsp_url = "file:///tmp/video.mp4";
    EXPECT_FALSE(source.start(config, error));
    EXPECT_NE(error.find("video.rtsp_url"), std::string::npos);
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Stopped);
}

TEST(RtspSource, ConnectsWithEmbeddedRtspCredentials) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server(skai::test::RtspTestServer::Codec::H264,
                                      "viewer", "secret");
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = "rtsp://viewer:secret@127.0.0.1:" +
                      std::to_string(server.port()) + "/test";
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    EXPECT_TRUE(frames.pop_for(std::chrono::seconds(3)).has_value());
    EXPECT_EQ(output.str().find("secret"), std::string::npos);
}

TEST(RtspSource, ConnectsWithConfiguredRtspCredentials) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server(skai::test::RtspTestServer::Codec::H264,
                                      "viewer", "secret");
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.username = "viewer";
    config.password = "secret";
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    EXPECT_TRUE(frames.pop_for(std::chrono::seconds(3)).has_value());
    EXPECT_EQ(output.str().find("secret"), std::string::npos);
}

TEST(RtspSource, RejectsIncorrectRtspCredentials) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server(skai::test::RtspTestServer::Codec::H264,
                                      "viewer", "secret");
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.username = "viewer";
    config.password = "wrong-secret";
    ASSERT_TRUE(source.start(config, error));
    EXPECT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
    EXPECT_EQ(output.str().find("wrong-secret"), std::string::npos);
    source.stop();
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Stopped);
}

TEST(RtspSource, ReleasesPipelineAndCanStartAgain) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    ASSERT_TRUE(frames.pop_for(std::chrono::seconds(3)).has_value());
    source.stop();
    while (frames.pop_for(std::chrono::milliseconds(0)).has_value()) {}
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    auto restarted = frames.pop_for(std::chrono::seconds(3));
    ASSERT_TRUE(restarted.has_value());
    EXPECT_EQ(restarted->sequence, 1U);
    source.stop();
}

TEST(RtspSource, ReportsUnhealthySourceAfterDisconnect) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    auto status = std::make_shared<skai::RuntimeStatus>();
    status->set_running(true);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software, true,
                            status);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    ASSERT_TRUE(frames.pop_for(std::chrono::seconds(3)).has_value()) << output.str();
    EXPECT_TRUE(status->snapshot().video_fps.has_value());
    server.stop();
    EXPECT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
    EXPECT_FALSE(source.diagnostics().last_error.empty());
    EXPECT_FALSE(status->snapshot().video_fps.has_value());
}

TEST(RtspVideoModule, PublishesToSharedInferenceQueueAfterRestart) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspVideoModule module(frames, logger);
    skai::Config config;
    config.video.rtsp_url = server.url();
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(module.initialize(config));
        ASSERT_TRUE(module.start()) << output.str();
        EXPECT_FALSE(frames.is_shutdown());
        auto frame = frames.pop_for(std::chrono::seconds(3));
        ASSERT_TRUE(frame.has_value()) << output.str();
        EXPECT_EQ(frame->sequence, 1U);
        module.stop();
        module.wait();
        EXPECT_TRUE(frames.is_shutdown());
        while (frames.pop_for(std::chrono::milliseconds(0)).has_value()) {}
    }
}

TEST(RtspRecovery, ConnectsWhenEndpointAppearsAfterOfflineStartup) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    const std::string configured_url = server.url();
    server.stop();
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = configured_url;
    config.reconnect_delay_ms = 100;
    config.stall_timeout_ms = 600;
    ASSERT_TRUE(source.start(config, error)) << error;
    ASSERT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
    ASSERT_TRUE(server.start(error)) << error;
    ASSERT_EQ(server.url(), configured_url);
    auto frame = frames.pop_for(std::chrono::seconds(6));
    ASSERT_TRUE(frame.has_value()) << source.diagnostics().last_error << output.str();
    EXPECT_TRUE(wait_for_health(source, skai::SourceHealth::Connected,
                                std::chrono::seconds(2)));
    EXPECT_GE(source.diagnostics().reconnect_count, 1U);
}

TEST(RtspRecovery, RecoversAfterServerRestartWithoutChangingUrl) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    const std::string configured_url = server.url();
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = configured_url;
    config.reconnect_delay_ms = 100;
    config.stall_timeout_ms = 600;
    ASSERT_TRUE(source.start(config, error)) << error;
    auto first = frames.pop_for(std::chrono::seconds(5));
    ASSERT_TRUE(first.has_value()) << output.str();
    server.stop();
    ASSERT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
    while (frames.pop_for(std::chrono::milliseconds(0)).has_value()) {}
    ASSERT_TRUE(server.start(error)) << error;
    ASSERT_EQ(server.url(), configured_url);
    auto recovered = frames.pop_for(std::chrono::seconds(6));
    ASSERT_TRUE(recovered.has_value()) << output.str();
    EXPECT_GT(recovered->sequence, first->sequence);
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Connected);
    EXPECT_GE(source.diagnostics().reconnect_count, 1U);
}

TEST(RtspRecovery, DetectsStallAndReconnectsWhenFramesResume) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.reconnect_delay_ms = 100;
    config.stall_timeout_ms = 600;
    ASSERT_TRUE(source.start(config, error)) << error;
    auto first = frames.pop_for(std::chrono::seconds(5));
    ASSERT_TRUE(first.has_value()) << output.str();
    ASSERT_TRUE(server.set_stalled(true));
    ASSERT_TRUE(wait_for_health(source, skai::SourceHealth::Stalled,
                                std::chrono::seconds(3))) << output.str();
    ASSERT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
    while (frames.pop_for(std::chrono::milliseconds(0)).has_value()) {}
    ASSERT_TRUE(server.set_stalled(false));
    auto recovered = frames.pop_for(std::chrono::seconds(6));
    ASSERT_TRUE(recovered.has_value()) << output.str();
    EXPECT_GT(recovered->sequence, first->sequence);
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Connected);
}

TEST(RtspRecovery, RebuildsDecodeChainWhenCodecChangesAfterRestart) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    if (!plugin_available("x265enc") || !plugin_available("avdec_h265")) {
        GTEST_SKIP() << "H.265 test plugins unavailable";
    }
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    const auto configured_url = server.url();
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = configured_url;
    config.reconnect_delay_ms = 100;
    config.stall_timeout_ms = 1000;
    ASSERT_TRUE(source.start(config, error));
    ASSERT_TRUE(frames.pop_for(std::chrono::seconds(5)).has_value()) << output.str();
    EXPECT_EQ(source.diagnostics().codec, "H264");
    server.stop();
    ASSERT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
    ASSERT_TRUE(server.set_codec(skai::test::RtspTestServer::Codec::H265));
    ASSERT_TRUE(server.start(error)) << error;
    ASSERT_EQ(server.url(), configured_url);
    ASSERT_TRUE(frames.pop_for(std::chrono::seconds(8)).has_value()) << output.str();
    EXPECT_EQ(source.diagnostics().codec, "H265");
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Connected);
}

TEST(RtspRecovery, StopsPromptlyWhileWaitingToReconnectWithoutFdGrowth) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    const std::string configured_url = server.url();
    server.stop();
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = configured_url;
    config.reconnect_delay_ms = 1000;
    std::size_t warmed_fd_count = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        ASSERT_TRUE(source.start(config, error)) << error;
        ASSERT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                    std::chrono::seconds(3))) << output.str();
        const auto before_stop = std::chrono::steady_clock::now();
        source.stop();
        EXPECT_LT(std::chrono::steady_clock::now() - before_stop,
                  std::chrono::seconds(3));
        EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Stopped);
        const auto current_fd_count = open_file_descriptors();
        if (cycle == 0) warmed_fd_count = current_fd_count;
        else EXPECT_LE(current_fd_count, warmed_fd_count + 1);
    }
}

TEST(RtspRecovery, RepeatedOfflineRetriesDoNotAccumulateFileDescriptors) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    const auto configured_url = server.url();
    server.stop();
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = configured_url;
    config.reconnect_delay_ms = 40;
    config.max_reconnect_delay_ms = 100;
    std::size_t warmed_fd_count = 0;
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(source.start(config, error)) << error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (source.diagnostics().reconnect_count < 5 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_GE(source.diagnostics().reconnect_count, 5U) << output.str();
        source.stop();
        const auto fd_count = open_file_descriptors();
        if (cycle == 0) warmed_fd_count = fd_count;
        else EXPECT_LE(fd_count, warmed_fd_count + 1);
    }
}

TEST(RtspRecovery, StopInterruptsAnRtspServerThatNeverResponds) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    HangingRtspEndpoint endpoint;
    ASSERT_GT(endpoint.port(), 0);
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = "rtsp://127.0.0.1:" + std::to_string(endpoint.port()) + "/test";
    ASSERT_TRUE(source.start(config, error)) << error;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!endpoint.accepted() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(endpoint.accepted()) << output.str();
    const auto before_stop = std::chrono::steady_clock::now();
    source.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before_stop,
              std::chrono::seconds(3)) << output.str();
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Stopped);
}

TEST(RtspRecovery, WaitsForInitialKeyframeLongerThanEstablishedFrameStall) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    ASSERT_TRUE(server.set_stalled(true));
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    config.stall_timeout_ms = 200;
    config.first_frame_timeout_ms = 1500;
    ASSERT_TRUE(source.start(config, error)) << error;
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Connecting) << output.str();
    EXPECT_EQ(source.diagnostics().reconnect_count, 0U);
    EXPECT_TRUE(wait_for_health(source, skai::SourceHealth::Reconnecting,
                                std::chrono::seconds(3))) << output.str();
}

TEST(RtspRecovery, RepeatedTeardownDuringRtspNegotiationIsSafe) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    for (int cycle = 0; cycle < 8; ++cycle) {
        ASSERT_TRUE(source.start(config, error)) << error;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        source.stop();
        EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Stopped);
    }
}

TEST(RtspSource, DiagnosticModeMeasuresFramesWithoutInferenceQueueDrops) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software, false);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    ASSERT_TRUE(source.start(config, error)) << error;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (source.diagnostics().frames_received < 5 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto metrics = source.diagnostics();
    ASSERT_GE(metrics.frames_received, 5U) << output.str();
    EXPECT_EQ(metrics.health, skai::SourceHealth::Connected);
    EXPECT_EQ(metrics.frames_dropped, 0U);
    EXPECT_EQ(metrics.frames_discarded, 0U);
    EXPECT_EQ(frames.size(), 0U);
}
