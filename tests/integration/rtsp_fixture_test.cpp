#include "rtsp_test_server.hpp"
#include "skai/video/gstreamer_runtime.hpp"

#include <gst/app/gstappsink.h>
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

bool receives_sample(const std::string& url, const std::string& depayloader,
                     skai::Logger& logger) {
    std::string error;
    auto client = skai::gst::Pipeline::from_launch(
        "rtspsrc location=" + url + " protocols=tcp latency=50 ! " + depayloader +
            " ! appsink name=sink sync=false max-buffers=1 drop=true",
        logger, error);
    if (!client || !client->start(std::chrono::seconds(6))) return false;
    GstElement* sink = gst_bin_get_by_name(GST_BIN(client->element()), "sink");
    if (!sink) return false;
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
    if (sample) gst_sample_unref(sample);
    gst_object_unref(sink);
    client->stop();
    return sample != nullptr;
}

} // namespace

TEST(RtspFixture, PublishesH264OnEphemeralLoopbackPort) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    EXPECT_GT(server.port(), 0);
    EXPECT_NE(server.url().find("rtsp://127.0.0.1:"), std::string::npos);
    std::ostringstream output;
    skai::Logger logger(output);
    EXPECT_TRUE(receives_sample(server.url(), "rtph264depay", logger)) << output.str();
    server.stop();
}

TEST(RtspFixture, CanStopAndRestart) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    for (int cycle = 0; cycle < 10; ++cycle) {
        ASSERT_TRUE(server.start(error)) << error;
        server.stop();
        EXPECT_EQ(server.port(), 0);
    }
    ASSERT_TRUE(server.start(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    EXPECT_TRUE(receives_sample(server.url(), "rtph264depay", logger)) << output.str();
}

TEST(RtspFixture, CanRestartWhileClientWasPlaying) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    auto client = skai::gst::Pipeline::from_launch(
        "rtspsrc location=" + server.url() +
            " protocols=tcp latency=50 ! rtph264depay ! "
            "appsink name=sink sync=false max-buffers=1 drop=true",
        logger, error);
    ASSERT_TRUE(client) << error;
    ASSERT_TRUE(client->start(std::chrono::seconds(6))) << client->last_error();
    GstElement* sink = gst_bin_get_by_name(GST_BIN(client->element()), "sink");
    ASSERT_NE(sink, nullptr);
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
    ASSERT_NE(sample, nullptr);
    gst_sample_unref(sample);
    gst_object_unref(sink);

    server.stop();
    EXPECT_EQ(server.port(), 0);
    client->stop();
    ASSERT_TRUE(server.start(error)) << error;
    EXPECT_TRUE(receives_sample(server.url(), "rtph264depay", logger)) << output.str();
}

TEST(RtspFixture, CanStallAndResumeActiveStream) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    auto client = skai::gst::Pipeline::from_launch(
        "rtspsrc location=" + server.url() +
            " protocols=tcp latency=0 ! rtph264depay ! "
            "appsink name=sink sync=false max-buffers=1 drop=true",
        logger, error);
    ASSERT_TRUE(client) << error;
    ASSERT_TRUE(client->start(std::chrono::seconds(6))) << client->last_error();
    GstElement* sink = gst_bin_get_by_name(GST_BIN(client->element()), "sink");
    ASSERT_NE(sink, nullptr);
    GstSample* first = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
    ASSERT_NE(first, nullptr);
    gst_sample_unref(first);

    ASSERT_TRUE(server.set_stalled(true));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    GstSample* stale = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0);
    if (stale) gst_sample_unref(stale);
    GstSample* blocked = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 300 * GST_MSECOND);
    EXPECT_EQ(blocked, nullptr);
    if (blocked) gst_sample_unref(blocked);

    ASSERT_TRUE(server.set_stalled(false));
    GstSample* resumed = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
    EXPECT_NE(resumed, nullptr);
    if (resumed) gst_sample_unref(resumed);
    gst_object_unref(sink);
    client->stop();
}

TEST(RtspFixture, PublishesH265WhenPluginsAreAvailable) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    if (!plugin_available("x265enc") || !plugin_available("rtph265pay") ||
        !plugin_available("rtph265depay")) GTEST_SKIP() << "H.265 test plugins unavailable";
    skai::test::RtspTestServer server(skai::test::RtspTestServer::Codec::H265);
    ASSERT_TRUE(server.start(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    EXPECT_TRUE(receives_sample(server.url(), "rtph265depay", logger)) << output.str();
}
