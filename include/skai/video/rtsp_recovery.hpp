#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace skai {

enum class SourceHealth { Stopped, Connecting, Connected, Degraded, Stalled,
                          Reconnecting, Error };

// Pure frame-freshness and retry policy; the GStreamer worker owns this state.
class RtspRecovery {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    RtspRecovery(std::chrono::milliseconds stall_timeout,
                 std::chrono::milliseconds reconnect_delay,
                 std::chrono::milliseconds max_reconnect_delay)
        : stall_timeout_(stall_timeout), reconnect_delay_(reconnect_delay),
          max_reconnect_delay_(max_reconnect_delay) {}

    void begin(TimePoint) {
        health_ = SourceHealth::Connecting;
        last_frame_.reset();
        degraded_until_.reset();
        failure_streak_ = 0;
        reconnect_count_ = 0;
    }

    void frame(TimePoint now) {
        health_ = degraded_until_ && now < *degraded_until_
                      ? SourceHealth::Degraded : SourceHealth::Connected;
        last_frame_ = now;
        failure_streak_ = 0;
    }

    void packet_loss(TimePoint now) {
        if (health_ == SourceHealth::Connected || health_ == SourceHealth::Degraded) {
            health_ = SourceHealth::Degraded;
            degraded_until_ = now + std::chrono::seconds(1);
        }
    }

    bool check_stall(TimePoint now) {
        if ((health_ == SourceHealth::Connected || health_ == SourceHealth::Degraded) &&
            last_frame_ && now - *last_frame_ >= stall_timeout_) {
            health_ = SourceHealth::Stalled;
            return true;
        }
        return false;
    }

    void fail(TimePoint now) {
        health_ = SourceHealth::Reconnecting;
        degraded_until_.reset();
        ++failure_streak_;
        auto delay = reconnect_delay_;
        for (std::uint32_t attempt = 1; attempt < failure_streak_; ++attempt) {
            delay = std::min(delay * 2, max_reconnect_delay_);
            if (delay >= max_reconnect_delay_) break;
        }
        next_retry_ = now + std::min(delay, max_reconnect_delay_);
    }

    bool retry_due(TimePoint now) const {
        return health_ == SourceHealth::Reconnecting && now >= next_retry_;
    }

    void retry(TimePoint now) {
        if (!retry_due(now)) return;
        health_ = SourceHealth::Connecting;
        ++reconnect_count_;
    }

    void stop() { health_ = SourceHealth::Stopped; }

    SourceHealth health() const { return health_; }
    std::uint64_t reconnect_count() const { return reconnect_count_; }
    TimePoint next_retry() const { return next_retry_; }
    std::optional<TimePoint> last_frame() const { return last_frame_; }

private:
    std::chrono::milliseconds stall_timeout_;
    std::chrono::milliseconds reconnect_delay_;
    std::chrono::milliseconds max_reconnect_delay_;
    SourceHealth health_ = SourceHealth::Stopped;
    std::optional<TimePoint> last_frame_;
    std::optional<TimePoint> degraded_until_;
    TimePoint next_retry_{};
    std::uint32_t failure_streak_ = 0;
    std::uint64_t reconnect_count_ = 0;
};

} // namespace skai
