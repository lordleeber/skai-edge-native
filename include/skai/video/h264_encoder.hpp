#pragma once

#include "skai/core/bounded_queue.hpp"
#include "skai/logging.hpp"
#include "skai/status.hpp"
#include "skai/video/encoded_access_unit.hpp"
#include "skai/video/frame.hpp"
#include "skai/video/gstreamer_runtime.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skai {

struct H264EncoderConfig {
    int bitrate_kbps = 2'000;
    int keyframe_interval = 30;
    int fps_num = 30;
    int fps_den = 1;
};

struct H264EncoderMetrics {
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_rejected = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t appsrc_pressure_dropped = 0;
    std::uint64_t access_units_encoded = 0;
    std::uint64_t bytes_encoded = 0;
    std::uint64_t access_units_dropped = 0;
    std::uint64_t pipeline_rebuilds = 0;
    std::int64_t last_access_unit_age_ms = -1;
    std::string last_error;
};

std::string serialize_h264_encoder_metrics(const H264EncoderMetrics& metrics);

// Encodes frames on a worker thread.  It owns the GStreamer pipeline but not
// either queue.  The output queue drops its oldest access unit when a consumer
// is slow, so recording or WebRTC can never stall capture/inference.
class H264Encoder {
public:
    using AccessUnitSink = std::function<void(const EncodedAccessUnit&)>;

    H264Encoder(BoundedQueue<Frame>& input, BoundedQueue<EncodedAccessUnit>& output,
                Logger& logger, std::shared_ptr<RuntimeStatus> status = {},
                AccessUnitSink access_unit_sink = {});
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // GStreamer must have been initialized before starting.  The pipeline is
    // created after the first valid frame establishes its dimensions.
    bool start(const H264EncoderConfig& config, std::string& error);
    void request_stop() noexcept;
    void stop() noexcept;
    H264EncoderMetrics metrics() const;

private:
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    bool open_pipeline(const Frame& first_frame, std::string& error);
    void close_pipeline() noexcept;
    bool submit_frame(const Frame& frame, bool& dropped, std::string& error);
    bool capture_sample(GstSample* sample);
    void run() noexcept;
    void set_error(const std::string& error);
    void publish_metrics();
    bool begin_callback();
    void end_callback();

    BoundedQueue<Frame>& input_;
    BoundedQueue<EncodedAccessUnit>& output_;
    Logger& logger_;
    std::shared_ptr<RuntimeStatus> status_;
    AccessUnitSink access_unit_sink_;
    H264EncoderConfig config_;
    std::unique_ptr<gst::Pipeline> pipeline_;
    GstElement* appsrc_ = nullptr; // borrowed from pipeline_; worker-only
    GstElement* appsink_ = nullptr; // borrowed from pipeline_; worker-only
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex callback_mutex_;
    std::condition_variable callback_changed_;
    bool accepting_callbacks_ = false;
    std::size_t callbacks_in_flight_ = 0;
    mutable std::mutex metrics_mutex_;
    H264EncoderMetrics metrics_;
    std::chrono::steady_clock::time_point last_access_unit_time_{};
    int width_ = 0;
    int height_ = 0;
    std::chrono::steady_clock::time_point timestamp_epoch_{};
    std::uint64_t last_input_pts_ns_ = 0;
    bool has_input_pts_ = false;
};

} // namespace skai
