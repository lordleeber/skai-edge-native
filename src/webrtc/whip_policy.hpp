#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
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

} // namespace skai
