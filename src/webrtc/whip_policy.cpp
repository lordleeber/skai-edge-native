#include "webrtc/whip_policy.hpp"

#include "skai/video/encoded_access_unit.hpp"

#include <algorithm>

namespace skai {

std::string whip_resource_url(const std::string& endpoint,
                              const std::string& location) {
    const auto authority_end = endpoint.find('/', 8);
    const auto origin = endpoint.substr(0, authority_end);
    if (location.empty()) {
        throw UnrecoverableWhipError("WHIP 201 omitted Location; automatic retry stopped");
    }
    if (location.front() == '/' && (location.size() < 2 || location[1] != '/')) {
        return origin + location;
    }
    if (location.rfind(origin + '/', 0) == 0) return location;
    if (location.find(':') == std::string::npos && location.front() != '/') {
        const auto path_end = endpoint.find_last_of('/');
        return endpoint.substr(0, path_end + 1) + location;
    }
    throw UnrecoverableWhipError(
        "WHIP Location has a different origin; automatic retry stopped");
}

void WhipPeerState::gathered() {
    std::lock_guard<std::mutex> lock(mutex_);
    gathered_ = true;
    changed_.notify_all();
}

void WhipPeerState::connected() {
    std::lock_guard<std::mutex> lock(mutex_);
    disconnected_at_.reset();
    changed_.notify_all();
}

void WhipPeerState::disconnected(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!disconnected_at_) disconnected_at_ = now;
    changed_.notify_all();
}

void WhipPeerState::failed() {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    changed_.notify_all();
}

bool WhipPeerState::is_disconnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return disconnected_at_.has_value();
}

bool WhipPeerState::should_reconnect(Clock::time_point now,
                                     Clock::duration grace) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_ || (disconnected_at_ && now - *disconnected_at_ >= grace);
}

bool WhipPeerState::wait_for_gathering(const std::atomic<bool>& stopping,
                                       Clock::duration timeout) {
    const auto deadline = Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!gathered_ && !failed_ && !stopping) {
        const auto next_check = std::min(deadline, Clock::now() +
                                        std::chrono::milliseconds(100));
        if (next_check <= Clock::now()) break;
        changed_.wait_until(lock, next_check);
    }
    return gathered_ && !failed_ && !stopping;
}

std::uint32_t WhipRtpClock::timestamp(std::uint64_t generation, bool has_pts,
                                      std::uint64_t pts_ns) {
    if (has_last_ && generation != generation_) last_pts_ticks_.reset();
    generation_ = generation;
    std::uint32_t next = has_last_ ? last_ + frame_ticks_ : start_;
    if (has_pts) {
        const auto ticks = h264_rtp_timestamp(pts_ns);
        if (has_last_ && !last_pts_ticks_) {
            // First PTS after a restart or after units without PTS.
            offset_ = next - ticks;
        } else if (last_pts_ticks_) {
            const auto step = static_cast<std::int32_t>(ticks - *last_pts_ticks_);
            // Learn the frame interval from consecutive units at 10 fps or faster.
            if (step > 0 && step <= 9000) frame_ticks_ = static_cast<std::uint32_t>(step);
        }
        next = ticks + offset_;
        last_pts_ticks_ = ticks;
    }
    last_ = next;
    has_last_ = true;
    return next;
}

} // namespace skai
