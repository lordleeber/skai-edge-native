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

    // `generation` is the unit's EncodedAccessUnit::source_generation, carried by
    // every unit, so a restart is seen even when its flagged unit was discarded.
    std::uint32_t timestamp(std::uint64_t generation, bool has_pts, std::uint64_t pts_ns);

private:
    std::uint32_t start_;
    std::uint32_t offset_ = 0;
    std::uint32_t last_ = 0;
    std::uint32_t frame_ticks_ = 3000;
    std::optional<std::uint32_t> last_pts_ticks_;
    std::uint64_t generation_ = 0;
    bool has_last_ = false;
};

} // namespace skai
