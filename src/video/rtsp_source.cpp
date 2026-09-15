#include "skai/video/rtsp_source.hpp"

#include <gst/app/gstappsink.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iterator>
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

} // namespace

RtspSource::RtspSource(BoundedQueue<Frame>& frames, Logger& logger, DecodeMode decode_mode)
    : frames_(frames), logger_(logger), decode_mode_(decode_mode) {}

RtspSource::~RtspSource() { stop(); }

bool RtspSource::start(const VideoConfig& config, std::string& error) {
    if (pipeline_) {
        error = "RTSP source is already running";
        return false;
    }
    if (!validate_video_config(config, error)) return false;
    pipeline_ = gst::Pipeline::create_empty(logger_, error);
    if (!pipeline_) return false;

    GstElement* source = gst_element_factory_make("rtspsrc", "rtsp_source");
    if (!source) {
        error = "GStreamer rtspsrc plugin is unavailable";
        stop();
        return false;
    }
    g_object_set(source, "location", config.rtsp_url.c_str(), "latency", config.latency_ms,
                 "protocols", config.transport == "tcp" ? GST_RTSP_LOWER_TRANS_TCP
                                                         : GST_RTSP_LOWER_TRANS_UDP,
                 nullptr);
    if (!config.username.empty()) {
        g_object_set(source, "user-id", config.username.c_str(), "user-pw",
                     config.password.c_str(), nullptr);
    }
    if (!gst_bin_add(GST_BIN(pipeline_->element()), source)) {
        gst_object_unref(source);
        error = "could not add rtspsrc to RTSP pipeline";
        stop();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_ = {};
        diagnostics_.health = SourceHealth::Connecting;
    }
    g_signal_connect(source, "pad-added", G_CALLBACK(on_rtp_pad), this);
    if (!pipeline_->start(std::chrono::seconds(6))) {
        const auto detail = diagnostics().last_error;
        error = !detail.empty() ? detail
                               : pipeline_->last_error();
        stop();
        return false;
    }
    try {
        running_ = true;
        worker_ = std::thread(&RtspSource::capture_loop, this);
    } catch (const std::exception& failure) {
        error = failure.what();
        stop();
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(diagnostics_mutex_);
        diagnostics_changed_.wait_for(lock, std::chrono::seconds(6), [this] {
            return diagnostics_.health == SourceHealth::Connected ||
                   diagnostics_.health == SourceHealth::Error;
        });
        if (diagnostics_.health != SourceHealth::Connected) {
            error = !diagnostics_.last_error.empty() ? diagnostics_.last_error
                                                     : "RTSP source did not receive a video frame before timeout";
        }
    }
    if (!error.empty()) {
        stop();
        return false;
    }
    error.clear();
    return true;
}

void RtspSource::stop() noexcept {
    request_stop();
    if (pipeline_) pipeline_->stop();
    if (worker_.joinable()) worker_.join();
    sink_ = nullptr;
    pipeline_.reset();
    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
    diagnostics_.health = SourceHealth::Stopped;
}

void RtspSource::request_stop() noexcept { running_ = false; }

RtspDiagnostics RtspSource::diagnostics() const {
    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
    return diagnostics_;
}

void RtspSource::set_error(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_.health = SourceHealth::Error;
        diagnostics_.last_error = error;
    }
    diagnostics_changed_.notify_all();
    logger_.log(LogLevel::Error, "video", error);
}

void RtspSource::on_rtp_pad(GstElement*, GstPad* pad, gpointer user_data) {
    static_cast<RtspSource*>(user_data)->connect_rtp_pad(pad);
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
    GstElement* elements[] = {depay, parser, decoder, converter, color, filter, sink};
    if (std::any_of(std::begin(elements), std::end(elements), [](GstElement* e) { return !e; })) {
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
    if (!gst_element_link_many(depay, parser, decoder, converter, color, filter, sink, nullptr)) {
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
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_.codec = codec;
        diagnostics_.decoder = decoder_name;
    }
    for (auto* element : elements) gst_element_sync_state_with_parent(element);
}

void RtspSource::capture_loop() noexcept {
    while (running_) {
        const auto event = pipeline_->poll(std::chrono::milliseconds(sink_ ? 0 : 100));
        if (event.type == gst::BusEventType::Error || event.type == gst::BusEventType::Eos) {
            set_error(event.detail);
            break;
        }
        GstElement* sink = sink_.load();
        if (!sink) continue;
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
        if (!sample) continue;
        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstVideoInfo info{};
        GstVideoFrame video_frame{};
        if (caps && buffer && gst_video_info_from_caps(&info, caps) &&
            gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) {
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
                frame.bgr.resize(static_cast<std::size_t>(frame.stride) * height);
                for (int row = 0; row < height; ++row) {
                    std::memcpy(frame.bgr.data() + static_cast<std::size_t>(row) * frame.stride,
                                source_pixels + static_cast<std::size_t>(row) * source_stride,
                                static_cast<std::size_t>(frame.stride));
                }
                {
                    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
                    frame.sequence = ++diagnostics_.frames_received;
                    diagnostics_.health = SourceHealth::Connected;
                    diagnostics_.width = width;
                    diagnostics_.height = height;
                    diagnostics_.fps_num = info.fps_n;
                    diagnostics_.fps_den = info.fps_d;
                }
                diagnostics_changed_.notify_all();
                frames_.push(std::move(frame));
            }
            gst_video_frame_unmap(&video_frame);
        }
        gst_sample_unref(sample);
    }
    running_ = false;
}

} // namespace skai
