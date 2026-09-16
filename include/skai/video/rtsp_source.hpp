#pragma once

#include "skai/config.hpp"
#include "skai/core/bounded_queue.hpp"
#include "skai/video/frame.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/rtsp_recovery.hpp"
#include "skai/video/rtsp_metrics.hpp"
#include "skai/status.hpp"

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skai {

enum class DecodeMode { Auto, Software };

struct RtspDiagnostics {
    SourceHealth health = SourceHealth::Stopped;
    std::string codec;
    std::string decoder;
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
    double fps_in = 0.0;
    bool url_configured = false; // true after a valid effective URL is accepted
    std::string transport;
    std::uint64_t frames_dropped = 0;
    std::uint64_t frames_discarded = 0;
    std::int64_t last_frame_age_ms = -1;
    std::uint64_t packets_lost = 0;
    std::uint64_t packets_late = 0;
    std::uint64_t avg_jitter_ns = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t reconnect_count = 0;
    std::string last_error;
};

std::string serialize_rtsp_metrics(const RtspDiagnostics& diagnostics);

class RtspSource {
public:
    RtspSource(BoundedQueue<Frame>& frames, Logger& logger,
               DecodeMode decode_mode = DecodeMode::Auto,
               bool enqueue_frames = true,
               std::shared_ptr<RuntimeStatus> status = {});
    ~RtspSource();

    RtspSource(const RtspSource&) = delete;
    RtspSource& operator=(const RtspSource&) = delete;

    bool start(const VideoConfig& config, std::string& error);
    void request_stop() noexcept;
    void stop() noexcept;
    RtspDiagnostics diagnostics() const;

private:
    static void on_rtp_pad(GstElement* source, GstPad* pad, gpointer user_data);
    static void on_new_manager(GstElement* source, GstElement* manager, gpointer user_data);
    static void on_new_jitterbuffer(GstElement* manager, GstElement* jitterbuffer,
                                    unsigned int session, unsigned int stream,
                                    gpointer user_data);
    void connect_rtp_pad(GstPad* pad);
    bool open_pipeline(std::string& error);
    void close_pipeline() noexcept;
    bool capture_sample(GstSample* sample, RtspRecovery& recovery);
    void capture_loop() noexcept;
    void set_error(const std::string& error);
    void publish_recovery(const RtspRecovery& recovery);
    void read_jitter_stats(RtspRecovery& recovery);
    bool begin_callback();
    void end_callback();

    BoundedQueue<Frame>& frames_;
    Logger& logger_;
    DecodeMode decode_mode_;
    bool enqueue_frames_;
    std::shared_ptr<RuntimeStatus> status_;
    VideoConfig config_;
    std::unique_ptr<gst::Pipeline> pipeline_;
    GstElement* rtsp_element_ = nullptr; // borrowed from pipeline_; worker-only
    std::atomic<GstElement*> sink_{nullptr}; // borrowed from pipeline_
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex backoff_mutex_;
    std::condition_variable backoff_changed_;
    std::mutex callback_mutex_;
    std::condition_variable callback_changed_;
    bool accepting_callbacks_ = false;
    std::size_t callbacks_in_flight_ = 0;
    mutable std::mutex diagnostics_mutex_;
    std::condition_variable diagnostics_changed_;
    RtspDiagnostics diagnostics_;
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::chrono::steady_clock::time_point prior_frame_time_{};
    std::mutex jitter_mutex_;
    GstElement* jitterbuffer_ = nullptr; // owned reference while active
    std::vector<GstElement*> managers_; // owned references until signal disconnect
    MonotonicCounter pipeline_packets_lost_;
    MonotonicCounter pipeline_packets_late_;
};

} // namespace skai
