#include "rtsp_test_server.hpp"
#include "skai/video/rtsp_source.hpp"
#include "skai/video/rtsp_video_module.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace {

bool plugin_available(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

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
    EXPECT_LE(frames.size(), 2U);
    source.stop();
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Stopped);
}

} // namespace

TEST(RtspSource, DecodesH264IntoBoundedInferenceQueue) {
    receives_decoded_frames(skai::test::RtspTestServer::Codec::H264, "H264");
}

TEST(RtspSource, DecodesH265IntoBoundedInferenceQueue) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    if (!plugin_available("x265enc") || !plugin_available("avdec_h265")) {
        GTEST_SKIP() << "H.265 test plugins unavailable";
    }
    receives_decoded_frames(skai::test::RtspTestServer::Codec::H265, "H265");
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
    EXPECT_FALSE(source.start(config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(output.str().find("wrong-secret"), std::string::npos);
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
    skai::RtspSource source(frames, logger, skai::DecodeMode::Software);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    server.stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (source.diagnostics().health == skai::SourceHealth::Connected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(source.diagnostics().health, skai::SourceHealth::Error) << output.str();
    EXPECT_FALSE(source.diagnostics().last_error.empty());
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
