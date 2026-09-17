#pragma once

#include "skai/config.hpp"
#include "skai/logging.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace rtc { class PeerConnection; }

namespace skai {

enum class CreateSessionError {
    None,
    Disabled,
    Capacity,
    InvalidOffer,
    GatheringTimeout,
    Internal
};

struct CreateSessionResult {
    CreateSessionError error = CreateSessionError::None;
    std::string session_id;
    std::string answer_sdp;
    std::string message;

    explicit operator bool() const noexcept { return error == CreateSessionError::None; }
};

class WebRtcSession : public std::enable_shared_from_this<WebRtcSession> {
public:
    ~WebRtcSession();

    std::string id() const { return id_; }
    void close() noexcept;

private:
    friend class WebRtcManager;
    explicit WebRtcSession(std::string id);
    static std::shared_ptr<WebRtcSession> create(std::string id);
    CreateSessionResult accept_offer(std::string_view offer,
                                     std::chrono::milliseconds timeout);
    bool stale(std::chrono::steady_clock::time_point now,
               std::chrono::milliseconds timeout) const;

    std::string id_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::chrono::steady_clock::time_point last_activity_;
    bool local_description_ready_ = false;
    bool gathering_complete_ = false;
    bool connected_ = false;
    bool failed_ = false;
    bool closed_ = false;
};

class WebRtcManager {
public:
    explicit WebRtcManager(Logger& logger,
        std::chrono::milliseconds stale_timeout = std::chrono::minutes(5),
        std::chrono::milliseconds gathering_timeout = std::chrono::seconds(5));
    ~WebRtcManager();

    WebRtcManager(const WebRtcManager&) = delete;
    WebRtcManager& operator=(const WebRtcManager&) = delete;

    void configure(const WebrtcConfig& config);
    CreateSessionResult create_session(std::string_view offer_sdp);
    bool close_session(std::string_view session_id);
    std::size_t cleanup_stale_sessions();
    std::size_t session_count() const;
    void shutdown() noexcept;

private:
    std::string make_session_id();
    void cleanup_loop();

    Logger& logger_;
    const std::chrono::milliseconds stale_timeout_;
    const std::chrono::milliseconds gathering_timeout_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::unordered_map<std::string, std::shared_ptr<WebRtcSession>> sessions_;
    std::thread cleanup_worker_;
    std::size_t max_peers_ = 0;
    bool enabled_ = false;
    bool stopping_ = true;
};

} // namespace skai
