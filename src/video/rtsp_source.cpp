#include "skai/video/rtsp_source.hpp"

#include <gst/app/gstappsink.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>

namespace skai {
namespace {

bool plugin_available(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

struct ElementNames {
    const char* depay;
    const char* parser;
    const char* software_decoder;
};

ElementNames elements_for(const std::string& codec) {
    return codec == "H265" ? ElementNames{"rtph265depay", "h265parse", "avdec_h265"}
                            : ElementNames{"rtph264depay", "h264parse", "avdec_h264"};
}

const char* health_name(SourceHealth health) {
    switch (health) {
    case SourceHealth::Stopped: return "stopped";
    case SourceHealth::Connecting: return "connecting";
    case SourceHealth::Connected: return "connected";
    case SourceHealth::Degraded: return "degraded";
    case SourceHealth::Stalled: return "stalled";
    case SourceHealth::Reconnecting: return "reconnecting";
    case SourceHealth::Error: return "error";
    }
    return "unknown";
}

} // namespace

std::string serialize_rtsp_metrics(const RtspDiagnostics& d) {
    std::ostringstream json;
    json << "{\"url_configured\":" << (d.url_configured ? "true" : "false")
         << ",\"connected\":" << ((d.health == SourceHealth::Connected ||
                                      d.health == SourceHealth::Degraded) ? "true" : "false")
         << ",\"health\":\"" << health_name(d.health) << "\""
         << ",\"transport\":\"" << d.transport << "\""
         << ",\"codec\":\"" << d.codec << "\""
         << ",\"width\":" << d.width << ",\"height\":" << d.height
         << ",\"fps_in\":" << d.fps_in
         << ",\"frames_received\":" << d.frames_received
         << ",\"frames_dropped\":" << d.frames_dropped
         << ",\"frames_discarded\":" << d.frames_discarded
         << ",\"last_frame_age_ms\":" << d.last_frame_age_ms
         << ",\"reconnect_count\":" << d.reconnect_count
         << ",\"packets_lost\":" << d.packets_lost
         << ",\"packets_late\":" << d.packets_late
         << ",\"avg_jitter_ns\":" << d.avg_jitter_ns << '}';
    return json.str();
}

RtspSource::RtspSource(BoundedQueue<Frame>& frames, Logger& logger, DecodeMode decode_mode,
                       bool enqueue_frames, std::shared_ptr<RuntimeStatus> status,
                       BoundedQueue<EncodedAccessUnit>* encoded_access_units,
                       AccessUnitSink access_unit_sink)
    : frames_(frames), logger_(logger), decode_mode_(decode_mode),
      enqueue_frames_(enqueue_frames), status_(std::move(status)),
      encoded_access_units_(encoded_access_units),
      access_unit_sink_(std::move(access_unit_sink)) {}

RtspSource::~RtspSource() { stop(); }

bool RtspSource::start(const VideoConfig& config, std::string& error) {
    if (worker_.joinable()) {
        error = "RTSP source is already running";
        return false;
    }
    if (!validate_video_config(config, error)) return false;
    if (!gst_is_initialized()) {
        error = "GStreamer must be initialized before the RTSP source";
        return false;
    }
    config_ = config;
    if (status_) status_->clear_video();
    sink_ = nullptr;
    encoded_sink_ = nullptr;
    access_unit_sequence_ = 0;
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_ = {};
        diagnostics_.health = SourceHealth::Connecting;
        diagnostics_.url_configured = !config.rtsp_url.empty();
        diagnostics_.transport = config.transport;
        last_frame_time_ = {};
        prior_frame_time_ = {};
    }
    try {
        running_ = true;
        worker_ = std::thread(&RtspSource::capture_loop, this);
    } catch (const std::exception& failure) {
        running_ = false;
        error = failure.what();
        return false;
    }
    error.clear();
    return true;
}

bool RtspSource::open_pipeline(std::string& error) {
    sink_ = nullptr;
    encoded_sink_ = nullptr;
    first_access_unit_ = true;
    pipeline_ = gst::Pipeline::create_empty(logger_, error);
    if (!pipeline_) return false;

    GstElement* source = gst_element_factory_make("rtspsrc", "rtsp_source");
    if (!source) {
        error = "GStreamer rtspsrc plugin is unavailable";
        close_pipeline();
        return false;
    }
    g_object_set(source, "location", config_.rtsp_url.c_str(), "latency", config_.latency_ms,
                 "protocols", config_.transport == "tcp" ? GST_RTSP_LOWER_TRANS_TCP
                                                          : GST_RTSP_LOWER_TRANS_UDP,
                 nullptr);
    if (!config_.username.empty()) {
        g_object_set(source, "user-id", config_.username.c_str(), "user-pw",
                     config_.password.c_str(), nullptr);
    }
    if (!gst_bin_add(GST_BIN(pipeline_->element()), source)) {
        gst_object_unref(source);
        error = "could not add rtspsrc to RTSP pipeline";
        close_pipeline();
        return false;
    }
    rtsp_element_ = source;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        accepting_callbacks_ = true;
    }
    g_signal_connect(source, "pad-added", G_CALLBACK(on_rtp_pad), this);
    g_signal_connect(source, "new-manager", G_CALLBACK(on_new_manager), this);
    if (!pipeline_->start(std::chrono::seconds(6), [this] { return !running_; })) {
        const auto detail = diagnostics().last_error;
        error = !detail.empty() ? detail : pipeline_->last_error();
        close_pipeline();
        return false;
    }
    error.clear();
    return true;
}

void RtspSource::stop() noexcept {
    request_stop();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_.health = SourceHealth::Stopped;
    }
    if (status_) status_->clear_video();
}

void RtspSource::request_stop() noexcept {
    running_ = false;
    backoff_changed_.notify_all();
}

void RtspSource::close_pipeline() noexcept {
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        accepting_callbacks_ = false;
    }
    if (rtsp_element_) g_signal_handlers_disconnect_by_data(rtsp_element_, this);
    {
        std::unique_lock<std::mutex> lock(callback_mutex_);
        callback_changed_.wait(lock, [this] { return callbacks_in_flight_ == 0; });
    }
    std::vector<GstElement*> managers;
    {
        std::lock_guard<std::mutex> lock(jitter_mutex_);
        managers.swap(managers_);
    }
    for (auto* manager : managers) {
        g_signal_handlers_disconnect_by_data(manager, this);
        gst_object_unref(manager);
    }
    if (pipeline_) pipeline_->stop();
    {
        std::lock_guard<std::mutex> lock(jitter_mutex_);
        if (jitterbuffer_) gst_object_unref(jitterbuffer_);
        jitterbuffer_ = nullptr;
        pipeline_packets_lost_.reset();
        pipeline_packets_late_.reset();
    }
    sink_ = nullptr;
    encoded_sink_ = nullptr;
    rtsp_element_ = nullptr;
    pipeline_.reset();
}

RtspDiagnostics RtspSource::diagnostics() const {
    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
    auto snapshot = diagnostics_;
    const auto queue_stats = frames_.stats();
    snapshot.frames_dropped = queue_stats.dropped;
    snapshot.frames_discarded = queue_stats.discarded;
    if (last_frame_time_ != std::chrono::steady_clock::time_point{}) {
        snapshot.last_frame_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_frame_time_).count();
    }
    return snapshot;
}

void RtspSource::set_error(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_.health = SourceHealth::Error;
        diagnostics_.last_error = error;
    }
    if (status_) status_->clear_video();
    diagnostics_changed_.notify_all();
    logger_.log(LogLevel::Error, "video", error);
}

void RtspSource::on_rtp_pad(GstElement*, GstPad* pad, gpointer user_data) {
    auto* self = static_cast<RtspSource*>(user_data);
    if (!self->begin_callback()) return;
    self->connect_rtp_pad(pad);
    self->end_callback();
}

void RtspSource::on_new_manager(GstElement*, GstElement* manager, gpointer user_data) {
    auto* self = static_cast<RtspSource*>(user_data);
    if (!self->begin_callback()) return;
    {
        std::lock_guard<std::mutex> lock(self->jitter_mutex_);
        self->managers_.push_back(GST_ELEMENT(gst_object_ref(manager)));
    }
    g_signal_connect(manager, "new-jitterbuffer", G_CALLBACK(on_new_jitterbuffer), user_data);
    self->end_callback();
}

void RtspSource::on_new_jitterbuffer(GstElement*, GstElement* jitterbuffer,
                                     unsigned int, unsigned int, gpointer user_data) {
    auto* self = static_cast<RtspSource*>(user_data);
    if (!self->begin_callback()) return;
    std::lock_guard<std::mutex> lock(self->jitter_mutex_);
    if (self->jitterbuffer_) gst_object_unref(self->jitterbuffer_);
    self->jitterbuffer_ = GST_ELEMENT(gst_object_ref(jitterbuffer));
    self->pipeline_packets_lost_.reset();
    self->pipeline_packets_late_.reset();
    self->end_callback();
}

bool RtspSource::begin_callback() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (!accepting_callbacks_) return false;
    ++callbacks_in_flight_;
    return true;
}

void RtspSource::end_callback() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (--callbacks_in_flight_ == 0) callback_changed_.notify_all();
}

void RtspSource::read_jitter_stats(RtspRecovery& recovery) {
    GstStructure* stats = nullptr;
    std::lock_guard<std::mutex> jitter_lock(jitter_mutex_);
    if (jitterbuffer_) g_object_get(jitterbuffer_, "stats", &stats, nullptr);
    if (!stats) return;
    std::uint64_t lost = 0, late = 0, jitter = 0;
    gst_structure_get_uint64(stats, "num-lost", &lost);
    gst_structure_get_uint64(stats, "num-late", &late);
    gst_structure_get_uint64(stats, "avg-jitter", &jitter);
    gst_structure_free(stats);
    const auto added_lost = pipeline_packets_lost_.observe(lost);
    const auto added_late = pipeline_packets_late_.observe(late);
    if (added_lost > 0) recovery.packet_loss(std::chrono::steady_clock::now());
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_.packets_lost += added_lost;
        diagnostics_.packets_late += added_late;
        diagnostics_.avg_jitter_ns = jitter;
    }
    if (recovery.health() == SourceHealth::Degraded) publish_recovery(recovery);
}

void RtspSource::connect_rtp_pad(GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps) return;
    if (gst_caps_is_any(caps) || gst_caps_is_empty(caps) || gst_caps_get_size(caps) == 0) {
        gst_caps_unref(caps);
        return;
    }
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    const char* encoding = gst_structure_get_string(structure, "encoding-name");
    const std::string codec = encoding ? encoding : "";
    const bool video_rtp = std::string(gst_structure_get_name(structure)) == "application/x-rtp" &&
                           (codec == "H264" || codec == "H265");
    gst_caps_unref(caps);
    if (!video_rtp || sink_) return;

    const auto names = elements_for(codec);
    const bool use_hardware = decode_mode_ == DecodeMode::Auto &&
                              plugin_available("nvv4l2decoder") &&
                              (plugin_available("nvvidconv") ||
                               plugin_available("nvvideoconvert"));
    const char* decoder_name = use_hardware ? "nvv4l2decoder" : names.software_decoder;
    const char* converter_name = use_hardware
                                     ? (plugin_available("nvvidconv") ? "nvvidconv" : "nvvideoconvert")
                                     : "identity";
    GstElement* depay = gst_element_factory_make(names.depay, nullptr);
    GstElement* parser = gst_element_factory_make(names.parser, nullptr);
    GstElement* decoder = gst_element_factory_make(decoder_name, nullptr);
    GstElement* converter = gst_element_factory_make(converter_name, nullptr);
    GstElement* color = gst_element_factory_make("videoconvert", nullptr);
    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", "inference_sink");
    const bool publish_h264 = codec == "H264" &&
                              (encoded_access_units_ || access_unit_sink_);
    GstElement* compressed_filter = publish_h264
                                        ? gst_element_factory_make("capsfilter", nullptr)
                                        : nullptr;
    GstElement* tee = publish_h264 ? gst_element_factory_make("tee", nullptr) : nullptr;
    GstElement* decode_queue = publish_h264 ? gst_element_factory_make("queue", nullptr) : nullptr;
    GstElement* encoded_queue = publish_h264 ? gst_element_factory_make("queue", nullptr) : nullptr;
    GstElement* encoded_sink = publish_h264
                                   ? gst_element_factory_make("appsink", "encoded_sink")
                                   : nullptr;
    std::vector<GstElement*> elements = {depay, parser, decoder, converter, color, filter, sink};
    if (publish_h264) {
        elements.insert(elements.end(), {compressed_filter, tee, decode_queue,
                                         encoded_queue, encoded_sink});
    }
    if (std::any_of(elements.begin(), elements.end(), [](GstElement* e) { return !e; })) {
        for (auto* element : elements) {
            if (element) gst_object_unref(element);
        }
        set_error("RTSP " + codec + " depay, parser, decoder, or color plugin is unavailable");
        return;
    }

    GstCaps* output_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                                               "BGR", nullptr);
    g_object_set(filter, "caps", output_caps, nullptr);
    gst_caps_unref(output_caps);
    g_object_set(sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    for (auto* element : elements) gst_bin_add(GST_BIN(pipeline_->element()), element);
    bool chain_linked = false;
    if (publish_h264) {
        GstCaps* encoded_caps = gst_caps_new_simple(
            "video/x-h264", "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au", nullptr);
        g_object_set(parser, "config-interval", -1, nullptr);
        g_object_set(compressed_filter, "caps", encoded_caps, nullptr);
        g_object_set(encoded_sink, "sync", FALSE, "max-buffers", 120,
                     "drop", TRUE, nullptr);
        gst_caps_unref(encoded_caps);
        chain_linked =
            gst_element_link_many(depay, parser, compressed_filter, tee, nullptr) &&
            gst_element_link_many(tee, decode_queue, decoder, converter, color,
                                  filter, sink, nullptr) &&
            gst_element_link_many(tee, encoded_queue, encoded_sink, nullptr);
    } else {
        chain_linked = gst_element_link_many(depay, parser, decoder, converter, color,
                                             filter, sink, nullptr);
    }
    if (!chain_linked) {
        set_error("could not link RTSP " + codec + " decode chain");
        return;
    }
    GstPad* depay_sink = gst_element_get_static_pad(depay, "sink");
    const auto linked = gst_pad_link(pad, depay_sink);
    gst_object_unref(depay_sink);
    if (linked != GST_PAD_LINK_OK) {
        set_error("could not link RTSP " + codec + " RTP pad");
        return;
    }
    sink_ = sink;
    encoded_sink_ = encoded_sink;
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_.codec = codec;
        diagnostics_.decoder = decoder_name;
    }
    for (auto* element : elements) gst_element_sync_state_with_parent(element);
}

void RtspSource::publish_recovery(const RtspRecovery& recovery) {
    const auto health = recovery.health();
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_.health = health;
        diagnostics_.reconnect_count = recovery.reconnect_count();
        if (health == SourceHealth::Connected) diagnostics_.last_error.clear();
    }
    if (status_ && health != SourceHealth::Connected &&
        health != SourceHealth::Degraded) {
        status_->clear_video();
    }
    diagnostics_changed_.notify_all();
}

bool RtspSource::capture_sample(GstSample* sample, RtspRecovery& recovery) {
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info{};
    GstVideoFrame video_frame{};
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
        !gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) return false;

    bool captured = false;
    const int width = GST_VIDEO_INFO_WIDTH(&info);
    const int height = GST_VIDEO_INFO_HEIGHT(&info);
    const int source_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
    const auto* source_pixels = static_cast<const std::uint8_t*>(
        GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0));
    if (width > 0 && height > 0 && source_stride >= width * 3) {
        Frame frame;
        frame.timestamp = std::chrono::steady_clock::now();
        frame.pts_ns = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))
                           ? GST_BUFFER_PTS(buffer) : 0;
        frame.width = width;
        frame.height = height;
        frame.stride = width * 3;
        if (enqueue_frames_) {
            frame.bgr.resize(static_cast<std::size_t>(frame.stride) * height);
            for (int row = 0; row < height; ++row) {
                std::memcpy(frame.bgr.data() + static_cast<std::size_t>(row) * frame.stride,
                            source_pixels + static_cast<std::size_t>(row) * source_stride,
                            static_cast<std::size_t>(frame.stride));
            }
        }
        recovery.frame(frame.timestamp);
        {
            std::lock_guard<std::mutex> lock(diagnostics_mutex_);
            frame.sequence = ++diagnostics_.frames_received;
            diagnostics_.width = width;
            diagnostics_.height = height;
            diagnostics_.fps_num = info.fps_n;
            diagnostics_.fps_den = info.fps_d;
            if (info.fps_n > 0 && info.fps_d > 0) {
                diagnostics_.fps_in = static_cast<double>(info.fps_n) / info.fps_d;
            } else if (prior_frame_time_ != std::chrono::steady_clock::time_point{}) {
                const auto seconds = std::chrono::duration<double>(frame.timestamp - prior_frame_time_).count();
                if (seconds > 0) diagnostics_.fps_in = 1.0 / seconds;
            }
            prior_frame_time_ = frame.timestamp;
            last_frame_time_ = frame.timestamp;
            if (status_ && diagnostics_.fps_in > 0.0) {
                status_->update_video(diagnostics_.fps_in);
            }
        }
        if (enqueue_frames_) frames_.push(std::move(frame));
        publish_recovery(recovery);
        captured = true;
    }
    gst_video_frame_unmap(&video_frame);
    return captured;
}

bool RtspSource::capture_access_unit(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo mapped{};
    if (!buffer || !gst_buffer_map(buffer, &mapped, GST_MAP_READ)) return false;
    EncodedAccessUnit unit;
    unit.sequence = ++access_unit_sequence_;
    if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
        unit.pts_ns = GST_BUFFER_PTS(buffer);
    } else if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(buffer))) {
        unit.pts_ns = GST_BUFFER_DTS(buffer);
    }
    unit.keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    unit.discontinuity = first_access_unit_ ||
                         GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT);
    unit.bytes.assign(mapped.data, mapped.data + mapped.size);
    gst_buffer_unmap(buffer, &mapped);
    first_access_unit_ = false;
    if (unit.bytes.empty()) return false;
    try {
        if (access_unit_sink_) access_unit_sink_(unit);
        if (encoded_access_units_) encoded_access_units_->push(std::move(unit));
    } catch (const std::exception& failure) {
        logger_.log(LogLevel::Error, "video", failure.what());
        return false;
    } catch (...) {
        logger_.log(LogLevel::Error, "video", "could not publish source H.264 access unit");
        return false;
    }
    return true;
}

void RtspSource::capture_loop() noexcept {
    RtspRecovery recovery(std::chrono::milliseconds(config_.stall_timeout_ms),
                          std::chrono::milliseconds(config_.reconnect_delay_ms),
                          std::chrono::milliseconds(config_.max_reconnect_delay_ms));
    recovery.begin(std::chrono::steady_clock::now());
    bool initial_attempt = true;
    while (running_) {
        if (!initial_attempt) {
            std::unique_lock<std::mutex> lock(backoff_mutex_);
            backoff_changed_.wait_until(lock, recovery.next_retry(), [this] {
                return !running_;
            });
            if (!running_) break;
            recovery.retry(std::chrono::steady_clock::now());
        }
        initial_attempt = false;
        {
            std::lock_guard<std::mutex> lock(diagnostics_mutex_);
            diagnostics_.codec.clear();
            diagnostics_.decoder.clear();
        }
        publish_recovery(recovery);

        std::string error;
        if (!open_pipeline(error)) {
            if (!running_) break;
            set_error(error);
            recovery.fail(std::chrono::steady_clock::now());
            publish_recovery(recovery);
            continue;
        }
        const auto attempt_started = std::chrono::steady_clock::now();
        bool received_frame = false;
        while (running_) {
            if (diagnostics().health == SourceHealth::Error) {
                error = diagnostics().last_error;
                break;
            }
            const auto event = pipeline_->poll(std::chrono::milliseconds(sink_ ? 0 : 100));
            if (event.type == gst::BusEventType::Error || event.type == gst::BusEventType::Eos) {
                error = event.detail;
                break;
            }
            GstElement* sink = sink_.load();
            GstElement* encoded_sink = encoded_sink_.load();
            if (encoded_sink) {
                while (GstSample* sample = gst_app_sink_try_pull_sample(
                           GST_APP_SINK(encoded_sink), 0)) {
                    capture_access_unit(sample);
                    gst_sample_unref(sample);
                }
            }
            if (sink) {
                GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                                  50 * GST_MSECOND);
                if (sample) {
                    received_frame = capture_sample(sample, recovery) || received_frame;
                    gst_sample_unref(sample);
                }
            }
            if (encoded_sink) {
                while (GstSample* sample = gst_app_sink_try_pull_sample(
                           GST_APP_SINK(encoded_sink), 0)) {
                    capture_access_unit(sample);
                    gst_sample_unref(sample);
                }
            }
            const auto now = std::chrono::steady_clock::now();
            read_jitter_stats(recovery);
            if (!received_frame && now - attempt_started >=
                                       std::chrono::milliseconds(config_.first_frame_timeout_ms)) {
                error = "RTSP source did not receive a video frame before timeout";
                break;
            }
            if (recovery.check_stall(now)) {
                publish_recovery(recovery);
                logger_.log(LogLevel::Warning, "video", "RTSP source stalled: no fresh frames");
                std::unique_lock<std::mutex> lock(backoff_mutex_);
                backoff_changed_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !running_;
                });
                error = "RTSP source stalled: no fresh frames";
                break;
            }
        }
        close_pipeline();
        if (!running_) break;
        frames_.discard_all();
        if (encoded_access_units_) encoded_access_units_->discard_all();
        set_error(error);
        recovery.fail(std::chrono::steady_clock::now());
        publish_recovery(recovery);
    }
    close_pipeline();
    recovery.stop();
    publish_recovery(recovery);
    running_ = false;
}

} // namespace skai
