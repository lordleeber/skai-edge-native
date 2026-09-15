#include "skai/video/gstreamer_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

namespace skai {
namespace gst {
namespace {

struct MessageDeleter {
    void operator()(GstMessage* message) const noexcept {
        if (message) gst_message_unref(message);
    }
};

using MessagePtr = std::unique_ptr<GstMessage, MessageDeleter>;

GstClockTime clock_time(std::chrono::milliseconds timeout) {
    return static_cast<GstClockTime>(std::max<std::int64_t>(0, timeout.count())) * GST_MSECOND;
}

} // namespace

bool initialize_once(std::string& error) {
    static std::once_flag once;
    static bool initialized = false;
    static std::string failure;
    std::call_once(once, [] {
        GError* gst_error = nullptr;
        initialized = gst_init_check(nullptr, nullptr, &gst_error);
        if (!initialized) {
            failure = gst_error ? gst_error->message : "GStreamer initialization failed";
        }
        if (gst_error) g_error_free(gst_error);
    });
    error = initialized ? std::string{} : failure;
    return initialized;
}

Pipeline::Pipeline(ElementPtr element, BusPtr bus, Logger& logger)
    : element_(std::move(element)), bus_(std::move(bus)), logger_(logger) {}

Pipeline::~Pipeline() {
    stop();
    bus_.reset();
    element_.reset();
}

std::unique_ptr<Pipeline> Pipeline::create_empty(Logger& logger, std::string& error) {
    if (!gst_is_initialized()) {
        error = "GStreamer must be initialized before creating a pipeline";
        return nullptr;
    }
    ElementPtr element(gst_pipeline_new(nullptr));
    if (!element) {
        error = "could not create GstPipeline";
        return nullptr;
    }
    BusPtr bus(gst_element_get_bus(element.get()));
    if (!bus) {
        error = "pipeline has no GstBus";
        return nullptr;
    }
    error.clear();
    return std::unique_ptr<Pipeline>(new Pipeline(std::move(element), std::move(bus), logger));
}

std::unique_ptr<Pipeline> Pipeline::from_launch(const std::string& launch,
                                                Logger& logger, std::string& error) {
    if (!gst_is_initialized()) {
        error = "GStreamer must be initialized before creating a pipeline";
        return nullptr;
    }
    GError* parse_error = nullptr;
    ElementPtr element(gst_parse_launch(launch.c_str(), &parse_error));
    if (parse_error) {
        error = parse_error->message;
        g_error_free(parse_error);
        return nullptr;
    }
    if (!element || !GST_IS_PIPELINE(element.get())) {
        error = "launch description must create a GstPipeline";
        return nullptr;
    }
    BusPtr bus(gst_element_get_bus(element.get()));
    if (!bus) {
        error = "pipeline has no GstBus";
        return nullptr;
    }
    error.clear();
    return std::unique_ptr<Pipeline>(new Pipeline(std::move(element), std::move(bus), logger));
}

bool Pipeline::start(std::chrono::milliseconds timeout) {
    last_error_.clear();
    gst_bus_set_flushing(bus_.get(), FALSE);
    stopped_ = false;
    logger_.log(LogLevel::Info, "gstreamer", "pipeline state PLAYING requested");
    if (gst_element_set_state(element_.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        last_error_ = "failed to request PLAYING state";
        logger_.log(LogLevel::Error, "gstreamer", last_error_);
        return false;
    }
    GstState state = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const auto result = gst_element_get_state(element_.get(), &state, &pending, clock_time(timeout));
    if (result == GST_STATE_CHANGE_FAILURE || state != GST_STATE_PLAYING) {
        last_error_ = result == GST_STATE_CHANGE_FAILURE
                          ? "pipeline failed while entering PLAYING"
                          : "pipeline did not enter PLAYING before timeout";
        logger_.log(LogLevel::Error, "gstreamer", last_error_);
        return false;
    }
    logger_.log(LogLevel::Info, "gstreamer", "pipeline entered PLAYING");
    return true;
}

BusEvent Pipeline::poll(std::chrono::milliseconds timeout) {
    const auto mask = static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
                                                   GST_MESSAGE_STATE_CHANGED);
    MessagePtr message(gst_bus_timed_pop_filtered(bus_.get(), clock_time(timeout), mask));
    if (!message) return {};

    if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_ERROR) {
        GError* gst_error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message.get(), &gst_error, &debug);
        const std::string detail = gst_error ? gst_error->message : "unknown GStreamer error";
        if (gst_error) g_error_free(gst_error);
        if (debug) g_free(debug);
        last_error_ = detail;
        logger_.log(LogLevel::Error, "gstreamer", detail);
        return {BusEventType::Error, detail};
    }
    if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_EOS) {
        logger_.log(LogLevel::Info, "gstreamer", "end of stream");
        return {BusEventType::Eos, "end of stream"};
    }
    GstState old_state = GST_STATE_VOID_PENDING;
    GstState new_state = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    gst_message_parse_state_changed(message.get(), &old_state, &new_state, &pending);
    if (GST_MESSAGE_SRC(message.get()) == GST_OBJECT(element_.get())) {
        logger_.log(LogLevel::Info, "gstreamer",
                    std::string("pipeline state ") + gst_element_state_get_name(old_state) +
                        " -> " + gst_element_state_get_name(new_state));
    }
    return {BusEventType::StateChanged, "state changed", old_state, new_state};
}

void Pipeline::stop() noexcept {
    if (stopped_ || !element_) return;
    stopped_ = true;
    logger_.log(LogLevel::Info, "gstreamer", "pipeline state NULL requested");
    const auto requested = gst_element_set_state(element_.get(), GST_STATE_NULL);
    GstState state = GST_STATE_VOID_PENDING;
    const auto settled = gst_element_get_state(element_.get(), &state, nullptr, 2 * GST_SECOND);
    if (requested == GST_STATE_CHANGE_FAILURE || settled == GST_STATE_CHANGE_FAILURE) {
        logger_.log(LogLevel::Error, "gstreamer", "pipeline failed while entering NULL");
    } else if (state == GST_STATE_NULL) {
        logger_.log(LogLevel::Info, "gstreamer", "pipeline entered NULL");
    } else {
        logger_.log(LogLevel::Warning, "gstreamer", "pipeline did not enter NULL before timeout");
    }
    if (bus_) gst_bus_set_flushing(bus_.get(), TRUE);
}

} // namespace gst
} // namespace skai
