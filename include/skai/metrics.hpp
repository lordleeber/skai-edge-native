#pragma once

#include "skai/core/bounded_queue.hpp"
#include "skai/video/rtsp_metrics.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skai {

// Counts delivered frames, so advertised camera framerate cannot mask loss.
class IngestRate {
public:
    std::optional<double> observe(std::chrono::steady_clock::time_point now) {
        if (!first_) {
            first_ = now;
            return std::nullopt;
        }
        ++frames_;
        const auto seconds = std::chrono::duration<double>(now - *first_).count();
        if (seconds <= 0) return std::nullopt;
        const double fps = static_cast<double>(frames_) / seconds;
        if (seconds >= 1.0) {
            first_ = now;
            frames_ = 0;
        }
        return fps;
    }
    void reset() noexcept { first_.reset(); frames_ = 0; }

private:
    std::optional<std::chrono::steady_clock::time_point> first_;
    std::uint64_t frames_ = 0;
};

struct SystemMetrics {
    std::optional<std::uint64_t> memory_rss_bytes;
    std::optional<double> cpu_percent;
    std::optional<double> gpu_percent;
    std::optional<std::uint64_t> disk_free_bytes;
};

struct WhipMetrics {
    bool enabled = false;
    std::string peer_state = "disabled";
    std::string ice_state = "disabled";
    std::string last_error;
    std::uint64_t access_units_sent = 0;
    std::size_t media_queue_drops = 0;
    std::size_t media_queue_discarded = 0;
};

struct MetricsSnapshot : SystemMetrics {
    std::optional<RtspDiagnostics> rtsp;
    QueueStats inference_queue;
    std::size_t inference_queue_depth = 0;
    QueueStats encoded_queue;
    std::size_t encoded_queue_depth = 0;
    std::size_t websocket_clients = 0;
    WhipMetrics whip;
    std::optional<std::uint64_t> alert_count;
    std::optional<double> encoder_fps;
    std::vector<std::string> recent_errors;
};

class SystemMetricsSampler {
public:
    explicit SystemMetricsSampler(std::filesystem::path disk_path)
        : disk_path_(std::move(disk_path)) {}
    SystemMetrics sample();

private:
    std::filesystem::path disk_path_;
    std::mutex mutex_;
    std::optional<std::chrono::steady_clock::time_point> previous_wall_;
    double previous_cpu_seconds_ = 0.0;
};

} // namespace skai
