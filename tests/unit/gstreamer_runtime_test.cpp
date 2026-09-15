#include "skai/video/gstreamer_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>

namespace {

void mark_finalized(gpointer data, GObject*) {
    static_cast<std::atomic<bool>*>(data)->store(true);
}

} // namespace

TEST(GStreamerRuntime, InitializesOnce) {
    std::string error;
    EXPECT_TRUE(skai::gst::initialize_once(error)) << error;
    EXPECT_TRUE(skai::gst::initialize_once(error)) << error;
    EXPECT_TRUE(gst_is_initialized());
}

TEST(GStreamerRuntime, PipelinePlaysHandlesEosAndLogsStateChanges) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    auto pipeline = skai::gst::Pipeline::from_launch(
        "videotestsrc num-buffers=3 ! fakesink sync=false", logger, error);
    ASSERT_TRUE(pipeline) << error;
    EXPECT_TRUE(pipeline->start(std::chrono::seconds(3))) << pipeline->last_error();

    bool saw_eos = false;
    bool saw_state_change = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !saw_eos) {
        const auto event = pipeline->poll(std::chrono::milliseconds(100));
        if (event.type == skai::gst::BusEventType::StateChanged) saw_state_change = true;
        if (event.type == skai::gst::BusEventType::Eos) saw_eos = true;
        EXPECT_NE(event.type, skai::gst::BusEventType::Error) << event.detail;
    }
    EXPECT_TRUE(saw_state_change);
    EXPECT_TRUE(saw_eos);
    EXPECT_NE(output.str().find("module=gstreamer"), std::string::npos);
    EXPECT_NE(output.str().find("state"), std::string::npos);
    EXPECT_NE(output.str().find("entered PLAYING"), std::string::npos);
}

TEST(GStreamerRuntime, ReportsErrorBusMessage) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    auto pipeline = skai::gst::Pipeline::from_launch(
        "videotestsrc num-buffers=3 ! identity error-after=1 ! fakesink sync=false",
        logger, error);
    ASSERT_TRUE(pipeline) << error;
    pipeline->start(std::chrono::seconds(2));

    bool saw_error = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !saw_error) {
        const auto event = pipeline->poll(std::chrono::milliseconds(100));
        if (event.type == skai::gst::BusEventType::Error) {
            saw_error = true;
            EXPECT_FALSE(event.detail.empty());
        }
    }
    EXPECT_TRUE(saw_error);
    EXPECT_NE(output.str().find("level=error"), std::string::npos);
}

TEST(GStreamerRuntime, ReleasesPipelineAndBusOnShutdown) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    std::atomic<bool> element_finalized{false};
    std::atomic<bool> bus_finalized{false};
    {
        auto pipeline = skai::gst::Pipeline::from_launch(
            "videotestsrc ! fakesink sync=false", logger, error);
        ASSERT_TRUE(pipeline) << error;
        g_object_weak_ref(G_OBJECT(pipeline->element()), mark_finalized, &element_finalized);
        g_object_weak_ref(G_OBJECT(pipeline->bus()), mark_finalized, &bus_finalized);
        ASSERT_TRUE(pipeline->start(std::chrono::seconds(2))) << pipeline->last_error();
        pipeline->stop();
        EXPECT_NE(output.str().find("entered NULL"), std::string::npos);
    }
    EXPECT_TRUE(element_finalized);
    EXPECT_TRUE(bus_finalized);
}
