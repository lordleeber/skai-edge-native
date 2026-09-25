#pragma once

#include "skai/video/rtsp_recovery.hpp"

#include <cstdint>
#include <string>

namespace skai {

inline const char* source_health_name(SourceHealth health) {
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

struct RtspDiagnostics {
    SourceHealth health = SourceHealth::Stopped;
    std::string codec;
    std::string decoder;
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
    double fps_in = 0.0;
    bool url_configured = false;
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

// Jitterbuffer stats are cumulative. Ignore temporary backwards readings;
// reset only when a new jitterbuffer starts its own counter epoch.
class MonotonicCounter {
public:
    std::uint64_t observe(std::uint64_t value) {
        if (value <= high_water_) return 0;
        const auto increase = value - high_water_;
        high_water_ = value;
        return increase;
    }

    void reset() { high_water_ = 0; }

private:
    std::uint64_t high_water_ = 0;
};

} // namespace skai
