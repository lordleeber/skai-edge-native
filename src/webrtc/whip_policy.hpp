#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace skai {

class UnrecoverableWhipError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A 201 without a safe cleanup URL must not start an automatic retry loop.
std::string whip_resource_url(const std::string& endpoint,
                              const std::string& location);

class WhipPeerState {
public:
    using Clock = std::chrono::steady_clock;

    void gathered();
    void connected();
    void disconnected(Clock::time_point now);
    void failed();
    bool is_disconnected() const;
    bool should_reconnect(Clock::time_point now,
                          Clock::duration grace) const;
    bool wait_for_gathering(const std::atomic<bool>& stopping,
                            Clock::duration timeout);

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Clock::time_point> disconnected_at_;
    bool gathered_ = false;
    bool failed_ = false;
};

// Maps access-unit PTS to WHIP RTP timestamps on one SSRC. PTS restarts near
// zero after an RTSP reconnect, so a discontinuity continues from the last sent
// timestamp plus one frame instead of jumping backward.
class WhipRtpClock {
public:
    explicit WhipRtpClock(std::uint32_t start_timestamp) : start_(start_timestamp) {}

    // Latched until the next sent unit, so keyframe gating cannot lose it.
    void mark_discontinuity() { rebase_ = true; }
    std::uint32_t timestamp(bool has_pts, std::uint64_t pts_ns);

private:
    std::uint32_t start_;
    std::uint32_t offset_ = 0;
    std::uint32_t last_ = 0;
    std::uint32_t frame_ticks_ = 3000;
    std::optional<std::uint32_t> last_pts_ticks_;
    bool has_last_ = false;
    bool rebase_ = false;
};

} // namespace skai
