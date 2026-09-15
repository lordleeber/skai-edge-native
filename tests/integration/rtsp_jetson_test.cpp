#include "rtsp_test_server.hpp"
#include "skai/video/rtsp_source.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>

namespace {

bool plugin_available(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

} // namespace

TEST(RtspJetson, UsesHardwareDecoderAndPublishesBgrFrames) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    if (!plugin_available("nvv4l2decoder") ||
        (!plugin_available("nvvidconv") && !plugin_available("nvvideoconvert"))) {
        GTEST_SKIP() << "Jetson hardware decode plugins unavailable";
    }
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::BoundedQueue<skai::Frame> frames(2);
    std::ostringstream output;
    skai::Logger logger(output);
    skai::RtspSource source(frames, logger);
    skai::VideoConfig config;
    config.rtsp_url = server.url();
    ASSERT_TRUE(source.start(config, error)) << error << output.str();
    auto frame = frames.pop_for(std::chrono::seconds(3));
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(source.diagnostics().decoder, "nvv4l2decoder");
    EXPECT_EQ(frame->bgr.size(), 160U * 120U * 3U);
}
