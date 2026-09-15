#pragma once

#include "skai/config.hpp"
#include "skai/core/bounded_queue.hpp"
#include "skai/video/gstreamer_runtime.hpp"

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

// Packed, CPU-addressable BGR pixels for the future inference consumer.
struct Frame {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp;
    std::uint64_t pts_ns = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> bgr;
};

enum class SourceHealth { Stopped, Connecting, Connected, Error };
enum class DecodeMode { Auto, Software };

struct RtspDiagnostics {
    SourceHealth health = SourceHealth::Stopped;
    std::string codec;
    std::string decoder;
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
    std::uint64_t frames_received = 0;
    std::string last_error;
};

class RtspSource {
public:
    RtspSource(BoundedQueue<Frame>& frames, Logger& logger,
               DecodeMode decode_mode = DecodeMode::Auto);
    ~RtspSource();

    RtspSource(const RtspSource&) = delete;
    RtspSource& operator=(const RtspSource&) = delete;

    bool start(const VideoConfig& config, std::string& error);
    void request_stop() noexcept;
    void stop() noexcept;
    RtspDiagnostics diagnostics() const;

private:
    static void on_rtp_pad(GstElement* source, GstPad* pad, gpointer user_data);
    void connect_rtp_pad(GstPad* pad);
    void capture_loop() noexcept;
    void set_error(const std::string& error);

    BoundedQueue<Frame>& frames_;
    Logger& logger_;
    DecodeMode decode_mode_;
    std::unique_ptr<gst::Pipeline> pipeline_;
    std::atomic<GstElement*> sink_{nullptr}; // borrowed from pipeline_
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex diagnostics_mutex_;
    std::condition_variable diagnostics_changed_;
    RtspDiagnostics diagnostics_;
};

} // namespace skai
