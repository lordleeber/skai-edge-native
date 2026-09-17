#include "skai/video/h264_encoder.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace skai {
namespace {

bool plugin_available(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

bool valid_config(const H264EncoderConfig& config, std::string& error) {
    if (config.bitrate_kbps <= 0 || config.keyframe_interval <= 0 ||
        config.fps_num <= 0 || config.fps_den <= 0) {
        error = "H.264 encoder bitrate, keyframe interval, and frame rate must be positive";
        return false;
    }
    if (config.bitrate_kbps > 2'048'000) {
        error = "H.264 encoder bitrate exceeds x264enc's supported maximum";
        return false;
    }
    return true;
}

bool valid_frame(const Frame& frame, std::string& error) {
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < 0 ||
        frame.width > std::numeric_limits<int>::max() / 3) {
        error = "encoder frame dimensions or stride are invalid";
        return false;
    }
    const auto row_bytes = static_cast<std::size_t>(frame.width) * 3;
    const auto stride = static_cast<std::size_t>(frame.stride);
    const auto rows_before_last = static_cast<std::size_t>(frame.height - 1);
    if (stride < row_bytes ||
        rows_before_last >
            (std::numeric_limits<std::size_t>::max() - row_bytes) / stride ||
        frame.bgr.size() < rows_before_last * stride + row_bytes) {
        error = "encoder frame buffer is shorter than its BGR dimensions and stride";
        return false;
    }
    return true;
}

std::string json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                escaped << "\\u00" << hex[(character >> 4) & 0x0f]
                        << hex[character & 0x0f];
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    return escaped.str();
}

} // namespace

std::string serialize_h264_encoder_metrics(const H264EncoderMetrics& metrics) {
    std::ostringstream json;
    json << "{\"frames_submitted\":" << metrics.frames_submitted
         << ",\"frames_rejected\":" << metrics.frames_rejected
         << ",\"frames_dropped\":" << metrics.frames_dropped
         << ",\"access_units_encoded\":" << metrics.access_units_encoded
         << ",\"bytes_encoded\":" << metrics.bytes_encoded
         << ",\"access_units_dropped\":" << metrics.access_units_dropped
         << ",\"last_access_unit_age_ms\":" << metrics.last_access_unit_age_ms
         << ",\"last_error\":\"" << json_escape(metrics.last_error) << "\"}";
    return json.str();
}

H264Encoder::H264Encoder(BoundedQueue<Frame>& input,
                         BoundedQueue<EncodedAccessUnit>& output,
                         Logger& logger)
    : input_(input), output_(output), logger_(logger) {}

H264Encoder::~H264Encoder() { stop(); }

bool H264Encoder::start(const H264EncoderConfig& config, std::string& error) {
    if (worker_.joinable()) {
        error = "H.264 encoder is already running";
        return false;
    }
    if (!valid_config(config, error)) return false;
    if (!gst_is_initialized()) {
        error = "GStreamer must be initialized before the H.264 encoder";
        return false;
    }
    if (!plugin_available("appsrc") || !plugin_available("videoconvert") ||
        !plugin_available("x264enc") || !plugin_available("h264parse") ||
        !plugin_available("appsink")) {
        error = "required GStreamer H.264 encoder plugins are unavailable";
        return false;
    }
    config_ = config;
    width_ = 0;
    height_ = 0;
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_ = {};
        last_access_unit_time_ = {};
    }
    output_.reset();
    running_ = true;
    try {
        worker_ = std::thread(&H264Encoder::run, this);
    } catch (const std::exception& exception) {
        running_ = false;
        error = exception.what();
        return false;
    }
    error.clear();
    return true;
}

void H264Encoder::request_stop() noexcept { running_ = false; }

void H264Encoder::stop() noexcept {
    request_stop();
    if (worker_.joinable()) worker_.join();
    output_.shutdown();
}

H264EncoderMetrics H264Encoder::metrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto snapshot = metrics_;
    snapshot.frames_dropped = input_.stats().dropped;
    snapshot.access_units_dropped = output_.stats().dropped;
    if (last_access_unit_time_ != std::chrono::steady_clock::time_point{}) {
        snapshot.last_access_unit_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_access_unit_time_).count();
    }
    return snapshot;
}

bool H264Encoder::open_pipeline(const Frame& first_frame, std::string& error) {
    pipeline_ = gst::Pipeline::create_empty(logger_, error);
    if (!pipeline_) return false;

    GstElement* source = gst_element_factory_make("appsrc", "encoder_input");
    GstElement* convert = gst_element_factory_make("videoconvert", "encoder_convert");
    GstElement* encoder = gst_element_factory_make("x264enc", "encoder_x264");
    GstElement* parser = gst_element_factory_make("h264parse", "encoder_parse");
    GstElement* sink = gst_element_factory_make("appsink", "encoder_output");
    if (!source || !convert || !encoder || !parser || !sink) {
        if (source) gst_object_unref(source);
        if (convert) gst_object_unref(convert);
        if (encoder) gst_object_unref(encoder);
        if (parser) gst_object_unref(parser);
        if (sink) gst_object_unref(sink);
        error = "could not create H.264 encoder pipeline elements";
        close_pipeline();
        return false;
    }
    GstCaps* raw_caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "BGR", "width", G_TYPE_INT,
        first_frame.width, "height", G_TYPE_INT, first_frame.height, "framerate",
        GST_TYPE_FRACTION, config_.fps_num, config_.fps_den, nullptr);
    GstCaps* encoded_caps = gst_caps_new_simple(
        "video/x-h264", "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
        G_TYPE_STRING, "au", nullptr);
    g_object_set(source, "caps", raw_caps, "is-live", TRUE, "format", GST_FORMAT_TIME,
                 "block", FALSE, "max-buffers", 2ULL, "leaky-type", 2, nullptr);
    g_object_set(encoder, "bitrate", config_.bitrate_kbps, "key-int-max",
                 config_.keyframe_interval, "speed-preset", 1, "tune", 4, nullptr);
    g_object_set(parser, "config-interval", -1, nullptr);
    g_object_set(sink, "caps", encoded_caps, "emit-signals", TRUE, "sync", FALSE,
                 "max-buffers", 1U, "drop", TRUE, nullptr);
    gst_caps_unref(raw_caps);
    gst_caps_unref(encoded_caps);

    gst_bin_add_many(GST_BIN(pipeline_->element()), source, convert, encoder, parser, sink,
                     nullptr);
    if (!gst_element_link_many(source, convert, encoder, parser, sink, nullptr)) {
        error = "could not link H.264 encoder pipeline";
        close_pipeline();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        accepting_callbacks_ = true;
    }
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), this);
    appsrc_ = source;
    appsink_ = sink;
    width_ = first_frame.width;
    height_ = first_frame.height;
    // An appsrc pipeline cannot preroll until an input buffer arrives.  Request
    // PLAYING here and let submit_frame provide that first buffer immediately,
    // rather than waiting for Pipeline::start's state-change timeout.
    logger_.log(LogLevel::Info, "encoder", "H.264 pipeline state PLAYING requested");
    if (gst_element_set_state(pipeline_->element(), GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        error = "failed to request PLAYING state for H.264 encoder pipeline";
        close_pipeline();
        return false;
    }
    return true;
}

void H264Encoder::close_pipeline() noexcept {
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        accepting_callbacks_ = false;
    }
    if (appsink_) g_signal_handlers_disconnect_by_data(appsink_, this);
    {
        std::unique_lock<std::mutex> lock(callback_mutex_);
        callback_changed_.wait(lock, [this] { return callbacks_in_flight_ == 0; });
    }
    appsrc_ = nullptr;
    appsink_ = nullptr;
    if (pipeline_) pipeline_->stop();
    pipeline_.reset();
}

bool H264Encoder::submit_frame(const Frame& frame, std::string& error) {
    if (!valid_frame(frame, error)) return false;
    if (frame.width != width_ || frame.height != height_) {
        error = "encoder frame dimensions changed after the pipeline started";
        return false;
    }
    const auto row_bytes = static_cast<std::size_t>(frame.width) * 3;
    const auto size = row_bytes * static_cast<std::size_t>(frame.height);
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buffer) {
        error = "could not allocate encoder input buffer";
        return false;
    }
    GstMapInfo mapped{};
    if (!gst_buffer_map(buffer, &mapped, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        error = "could not map encoder input buffer";
        return false;
    }
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(mapped.data + static_cast<std::size_t>(row) * row_bytes,
                    frame.bgr.data() + static_cast<std::size_t>(row) * frame.stride,
                    row_bytes);
    }
    gst_buffer_unmap(buffer, &mapped);
    GST_BUFFER_PTS(buffer) = frame.pts_ns;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(
        GST_SECOND, config_.fps_den, config_.fps_num);
    GST_BUFFER_OFFSET(buffer) = frame.sequence;
    GST_BUFFER_OFFSET_END(buffer) = frame.sequence;
    const auto result = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if (result != GST_FLOW_OK) {
        error = "H.264 encoder rejected input frame";
        return false;
    }
    return true;
}

bool H264Encoder::capture_sample(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) return false;
    GstMapInfo mapped{};
    if (!gst_buffer_map(buffer, &mapped, GST_MAP_READ)) return false;
    EncodedAccessUnit unit;
    unit.sequence = GST_BUFFER_OFFSET_IS_VALID(buffer) ? GST_BUFFER_OFFSET(buffer) : 0;
    unit.pts_ns = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) ?
                      GST_BUFFER_PTS(buffer) : 0;
    unit.keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    unit.bytes.assign(mapped.data, mapped.data + mapped.size);
    gst_buffer_unmap(buffer, &mapped);
    if (unit.bytes.empty()) return false;
    output_.push(std::move(unit));
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.access_units_encoded;
        metrics_.bytes_encoded += mapped.size;
        last_access_unit_time_ = std::chrono::steady_clock::now();
    }
    return true;
}

GstFlowReturn H264Encoder::on_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<H264Encoder*>(user_data);
    if (!self->begin_callback()) return GST_FLOW_FLUSHING;
    GstSample* sample = gst_app_sink_pull_sample(sink);
    const bool captured = sample && self->capture_sample(sample);
    if (sample) gst_sample_unref(sample);
    self->end_callback();
    return captured ? GST_FLOW_OK : GST_FLOW_ERROR;
}

bool H264Encoder::begin_callback() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (!accepting_callbacks_) return false;
    ++callbacks_in_flight_;
    return true;
}

void H264Encoder::end_callback() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (--callbacks_in_flight_ == 0) callback_changed_.notify_all();
}

void H264Encoder::set_error(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.last_error = error;
    }
    logger_.log(LogLevel::Error, "encoder", error);
}

void H264Encoder::run() noexcept {
    while (running_) {
        auto frame = input_.pop_for(std::chrono::milliseconds(25));
        if (!frame) {
            if (input_.is_shutdown()) break;
            continue;
        }
        std::string error;
        if (!valid_frame(*frame, error)) {
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.frames_rejected;
            }
            set_error(error);
            continue;
        }
        if (pipeline_) {
            const auto event = pipeline_->poll(std::chrono::milliseconds(0));
            if (event.type == gst::BusEventType::Error) {
                set_error(event.detail);
                close_pipeline();
            }
        }
        if (!pipeline_ && !open_pipeline(*frame, error)) {
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.frames_rejected;
            }
            set_error(error);
            continue;
        }
        if (!submit_frame(*frame, error)) {
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.frames_rejected;
            }
            set_error(error);
            continue;
        }
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.frames_submitted;
    }
    close_pipeline();
}

} // namespace skai
