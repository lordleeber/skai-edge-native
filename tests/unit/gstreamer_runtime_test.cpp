#include "skai/video/gstreamer_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace {

struct FailNullElement { GstElement parent; bool fail_once; };
struct FailNullElementClass { GstElementClass parent_class; };

G_DEFINE_TYPE(FailNullElement, fail_null_element, GST_TYPE_ELEMENT)

GstStateChangeReturn fail_null_change_state(GstElement* element,
                                            GstStateChange transition) {
    auto* self = reinterpret_cast<FailNullElement*>(element);
    if (transition == GST_STATE_CHANGE_READY_TO_NULL && self->fail_once) {
        self->fail_once = false;
        return GST_STATE_CHANGE_FAILURE;
    }
    return GST_ELEMENT_CLASS(fail_null_element_parent_class)->change_state(element,
                                                                           transition);
}

void fail_null_element_class_init(FailNullElementClass* klass) {
    GST_ELEMENT_CLASS(klass)->change_state = fail_null_change_state;
}

void fail_null_element_init(FailNullElement* element) { element->fail_once = true; }

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

TEST(GStreamerRuntime, CancelsAnAsynchronousStartWithoutWaitingForTimeout) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    auto pipeline = skai::gst::Pipeline::from_launch(
        "appsrc is-live=false ! fakesink sync=false", logger, error);
    ASSERT_TRUE(pipeline) << error;
    std::atomic<bool> cancel{false};
    std::thread trigger([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel = true;
    });
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(pipeline->start(std::chrono::seconds(6), [&] { return cancel.load(); }));
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(2));
    trigger.join();
}

TEST(GStreamerRuntime, FailedNullTransitionCanBeRetried) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    auto pipeline = skai::gst::Pipeline::from_launch(
        "videotestsrc is-live=true ! fakesink sync=false", logger, error);
    ASSERT_TRUE(pipeline) << error;
    auto* child = GST_ELEMENT(g_object_new(fail_null_element_get_type(), nullptr));
    ASSERT_TRUE(gst_bin_add(GST_BIN(pipeline->element()), child));
    ASSERT_TRUE(pipeline->start(std::chrono::seconds(2))) << pipeline->last_error();
    EXPECT_FALSE(pipeline->stop());
    EXPECT_NE(pipeline->last_error().find("NULL"), std::string::npos);
    EXPECT_TRUE(pipeline->stop());
    GstState state = GST_STATE_VOID_PENDING;
    gst_element_get_state(pipeline->element(), &state, nullptr, 0);
    EXPECT_EQ(state, GST_STATE_NULL);
}

TEST(GStreamerRuntime, DestructorRetriesAFailedNullTransition) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    std::ostringstream output;
    skai::Logger logger(output);
    auto pipeline = skai::gst::Pipeline::from_launch(
        "videotestsrc is-live=true ! fakesink sync=false", logger, error);
    ASSERT_TRUE(pipeline) << error;
    auto* child = GST_ELEMENT(g_object_new(fail_null_element_get_type(), nullptr));
    ASSERT_TRUE(gst_bin_add(GST_BIN(pipeline->element()), child));
    gst_object_ref(child);
    EXPECT_TRUE(pipeline->start(std::chrono::seconds(2))) << pipeline->last_error();
    EXPECT_FALSE(pipeline->stop());
    pipeline.reset();
    GstState state = GST_STATE_VOID_PENDING;
    gst_element_get_state(child, &state, nullptr, 0);
    EXPECT_EQ(state, GST_STATE_NULL);
    gst_object_unref(child);
}
