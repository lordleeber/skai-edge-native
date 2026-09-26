#include "skai/video/h264_encoder.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <algorithm>
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
    constexpr int level_3_1_max_bitrate_kbps = 14'000;
    if (config.bitrate_kbps > level_3_1_max_bitrate_kbps) {
        error = "H.264 encoder bitrate exceeds constrained-baseline Level 3.1";
        return false;
    }
    return true;
}

bool level_3_1_supports(const Frame& frame, const H264EncoderConfig& config,
                        std::string& error) {
    constexpr std::int64_t max_macroblocks_per_frame = 3'600;
    constexpr std::int64_t max_macroblocks_per_second = 108'000;
    const auto macroblocks_wide = (static_cast<std::int64_t>(frame.width) + 15) / 16;
    const auto macroblocks_high = (static_cast<std::int64_t>(frame.height) + 15) / 16;
    const auto macroblocks = macroblocks_wide * macroblocks_high;
    if (macroblocks > max_macroblocks_per_frame ||
        macroblocks * config.fps_num >
            max_macroblocks_per_second * config.fps_den) {
        error = "encoder frame dimensions and rate exceed constrained-baseline Level 3.1";
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
         << ",\"appsrc_pressure_dropped\":" << metrics.appsrc_pressure_dropped
         << ",\"access_units_encoded\":" << metrics.access_units_encoded
         << ",\"bytes_encoded\":" << metrics.bytes_encoded
         << ",\"access_units_dropped\":" << metrics.access_units_dropped
         << ",\"pipeline_rebuilds\":" << metrics.pipeline_rebuilds
         << ",\"last_access_unit_age_ms\":" << metrics.last_access_unit_age_ms
         << ",\"last_error\":\"" << json_escape(metrics.last_error) << "\"}";
    return json.str();
}

H264Encoder::H264Encoder(BoundedQueue<Frame>& input,
                         BoundedQueue<EncodedAccessUnit>& output,
                         Logger& logger, std::shared_ptr<RuntimeStatus> status,
                         AccessUnitSink access_unit_sink)
    : input_(input), output_(output), logger_(logger), status_(std::move(status)),
      access_unit_sink_(std::move(access_unit_sink)) {}

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
    timestamp_epoch_ = {};
    last_input_pts_ns_ = 0;
    has_input_pts_ = false;
    next_access_unit_discontinuity_ = true;
    {
        std::lock_guard<std::mutex> lock(timeline_mutex_);
        has_output_pts_ = false;
        has_pipeline_output_origin_ = false;
        last_output_pts_ns_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_ = {};
        last_access_unit_time_ = {};
    }
    output_.reset();
    publish_metrics();
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
    publish_metrics();
}

H264EncoderMetrics H264Encoder::metrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto snapshot = metrics_;
    snapshot.frames_dropped = input_.stats().dropped + snapshot.appsrc_pressure_dropped;
    snapshot.access_units_dropped = output_.stats().dropped;
    if (last_access_unit_time_ != std::chrono::steady_clock::time_point{}) {
        snapshot.last_access_unit_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_access_unit_time_).count();
    }
    return snapshot;
}

bool H264Encoder::open_pipeline(const Frame& first_frame, std::string& error) {
    if (!level_3_1_supports(first_frame, config_, error)) return false;
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
        G_TYPE_STRING, "au", "profile", G_TYPE_STRING, "constrained-baseline",
        "level", G_TYPE_STRING, "3.1", nullptr);
    g_object_set(source, "caps", raw_caps, "is-live", TRUE, "format", GST_FORMAT_TIME,
                 "block", FALSE, "max-buffers", 2ULL, "max-bytes", 0ULL,
                 "max-time", 0ULL, "leaky-type", 0, nullptr);
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
    if (timestamp_epoch_ == std::chrono::steady_clock::time_point{}) {
        timestamp_epoch_ = first_frame.timestamp == std::chrono::steady_clock::time_point{}
                               ? std::chrono::steady_clock::now()
                               : first_frame.timestamp;
    }
    {
        std::lock_guard<std::mutex> lock(timeline_mutex_);
        has_pipeline_output_origin_ = false;
    }
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
    next_access_unit_discontinuity_ = true;
    if (pipeline_) pipeline_->stop();
    pipeline_.reset();
}

bool H264Encoder::submit_frame(const Frame& frame, bool& dropped, std::string& error) {
    dropped = false;
    if (!valid_frame(frame, error)) return false;
    if (frame.width != width_ || frame.height != height_) {
        error = "encoder frame dimensions changed after the pipeline started";
        return false;
    }
    guint64 queued_buffers = 0;
    g_object_get(appsrc_, "current-level-buffers", &queued_buffers, nullptr);
    if (queued_buffers >= 2) {
        dropped = true;
        return true;
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
    const auto capture_time = frame.timestamp == std::chrono::steady_clock::time_point{}
                                  ? std::chrono::steady_clock::now()
                                  : frame.timestamp;
    const auto elapsed = capture_time > timestamp_epoch_
                             ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   capture_time - timestamp_epoch_).count()
                             : 0;
    auto pts_ns = static_cast<std::uint64_t>(elapsed);
    if (has_input_pts_ && pts_ns <= last_input_pts_ns_) {
        pts_ns = last_input_pts_ns_ + 1;
    }
    last_input_pts_ns_ = pts_ns;
    has_input_pts_ = true;
    GST_BUFFER_PTS(buffer) = pts_ns;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
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
    unit.queued_at = std::chrono::steady_clock::now();
    unit.sequence = GST_BUFFER_OFFSET_IS_VALID(buffer) ? GST_BUFFER_OFFSET(buffer) : 0;
    unit.has_pts = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer));
    unit.pts_ns = unit.has_pts ? GST_BUFFER_PTS(buffer) : 0;
    unit.has_dts = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(buffer));
    unit.dts_ns = unit.has_dts ? GST_BUFFER_DTS(buffer) : 0;
    unit.keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    unit.bytes.assign(mapped.data, mapped.data + mapped.size);
    gst_buffer_unmap(buffer, &mapped);
    if (unit.bytes.empty()) return false;
    if (unit.has_pts) {
        std::lock_guard<std::mutex> lock(timeline_mutex_);
        if (!has_pipeline_output_origin_) {
            // x264 can reuse its initial PTS offset after a pipeline rebuild.
            // Preserve deltas within this pipeline on the outgoing timeline.
            pipeline_raw_pts_origin_ns_ = unit.pts_ns;
            const auto frame_duration_ns = gst_util_uint64_scale_int(
                GST_SECOND, config_.fps_den, config_.fps_num);
            pipeline_output_pts_origin_ns_ = has_output_pts_
                ? std::max(unit.pts_ns, last_output_pts_ns_ + frame_duration_ns)
                : unit.pts_ns;
            has_pipeline_output_origin_ = true;
        }
        const auto rebase = [&](std::uint64_t raw) {
            if (raw >= pipeline_raw_pts_origin_ns_) {
                return pipeline_output_pts_origin_ns_ +
                       (raw - pipeline_raw_pts_origin_ns_);
            }
            const auto delta = pipeline_raw_pts_origin_ns_ - raw;
            return pipeline_output_pts_origin_ns_ > delta
                ? pipeline_output_pts_origin_ns_ - delta : std::uint64_t{0};
        };
        unit.pts_ns = rebase(unit.pts_ns);
        if (unit.has_dts) unit.dts_ns = rebase(unit.dts_ns);
        last_output_pts_ns_ = unit.pts_ns;
        has_output_pts_ = true;
    }
    unit.discontinuity = next_access_unit_discontinuity_.exchange(false);
    if (access_unit_sink_) {
        try {
            access_unit_sink_(unit);
        } catch (const std::exception& exception) {
            logger_.log(LogLevel::Error, "encoder",
                        "encoded access-unit sink failed: " + std::string(exception.what()));
        } catch (...) {
            logger_.log(LogLevel::Error, "encoder", "encoded access-unit sink failed");
        }
    }
    output_.push(std::move(unit));
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.access_units_encoded;
        metrics_.bytes_encoded += mapped.size;
        last_access_unit_time_ = std::chrono::steady_clock::now();
    }
    publish_metrics();
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
    publish_metrics();
}

void H264Encoder::publish_metrics() {
    if (!status_) return;
    const auto metrics_snapshot = metrics();
    RuntimeStatusSnapshot::Encoder encoder;
    encoder.frames_submitted = metrics_snapshot.frames_submitted;
    encoder.frames_rejected = metrics_snapshot.frames_rejected;
    encoder.frames_dropped = metrics_snapshot.frames_dropped;
    encoder.appsrc_pressure_dropped = metrics_snapshot.appsrc_pressure_dropped;
    encoder.access_units_encoded = metrics_snapshot.access_units_encoded;
    encoder.bytes_encoded = metrics_snapshot.bytes_encoded;
    encoder.access_units_dropped = metrics_snapshot.access_units_dropped;
    encoder.last_access_unit_age_ms = metrics_snapshot.last_access_unit_age_ms;
    encoder.last_error = metrics_snapshot.last_error;
    status_->update_encoder(encoder);
}

void H264Encoder::run() noexcept {
    while (running_) {
        auto frame = input_.pop_for(std::chrono::milliseconds(25));
        if (!frame) {
            if (input_.is_shutdown()) break;
            publish_metrics();
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
        if (pipeline_ && (frame->width != width_ || frame->height != height_)) {
            close_pipeline();
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.pipeline_rebuilds;
                metrics_.last_error.clear();
            }
            publish_metrics();
        }
        if (!pipeline_ && !open_pipeline(*frame, error)) {
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.frames_rejected;
            }
            set_error(error);
            continue;
        }
        bool dropped = false;
        if (!submit_frame(*frame, dropped, error)) {
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.frames_rejected;
            }
            set_error(error);
            close_pipeline();
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            if (dropped) ++metrics_.appsrc_pressure_dropped;
            else ++metrics_.frames_submitted;
        }
        publish_metrics();
    }
    close_pipeline();
}

} // namespace skai
