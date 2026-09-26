#pragma once

#include "skai/config.hpp"
#include "skai/profiling.hpp"
#include "skai/core/bounded_queue.hpp"
#include "skai/logging.hpp"
#include "skai/video/encoded_access_unit.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rtc {
class PeerConnection;
class RtcpSrReporter;
class RtpPacketizationConfig;
class Track;
}

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

struct WebRtcPeerDiagnostics {
    std::string session_id;
    std::string peer_state;
    std::string ice_state;
    std::string local_candidate;
    std::string local_interface;
    std::string selected_interface;
    std::uint64_t connection_age_s = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_retransmitted = 0;
    std::uint64_t media_queue_drops = 0;
    TimingSummary media_queue_wait;
    std::uint64_t keyframe_events = 0;
    std::string failure_stage;
    std::string close_reason;
    std::string last_error;
};

struct WebRtcDiagnostics {
    bool enabled = false;
    bool media_available = true;
    bool keyframe_cached = false;
    std::string media_unavailable_reason;
    bool lan_only = true;
    std::size_t max_peers = 0;
    std::uint64_t sessions_created = 0;
    std::uint64_t sessions_closed = 0;
    std::uint64_t signaling_errors = 0;
    std::uint64_t media_errors = 0;
    std::string last_close_reason;
    std::vector<WebRtcPeerDiagnostics> peers;
    std::vector<WebRtcPeerDiagnostics> recently_closed;
};

class WebRtcSession : public std::enable_shared_from_this<WebRtcSession> {
public:
    ~WebRtcSession();

    std::string id() const { return id_; }
    void close() noexcept;

private:
    friend class WebRtcManager;
    explicit WebRtcSession(std::string id, std::size_t queue_capacity,
                           std::atomic<std::uint64_t>* media_errors);
    static std::shared_ptr<WebRtcSession> create(std::string id,
                                                 std::size_t queue_capacity,
                                                 std::atomic<std::uint64_t>* media_errors);
    CreateSessionResult accept_offer(std::string_view offer,
                                     std::chrono::milliseconds timeout);
    WebRtcPeerDiagnostics diagnostics() const;
    std::string stale_reason(std::chrono::steady_clock::time_point now,
                             std::chrono::milliseconds timeout) const;
    void close_with_reason(std::string reason) noexcept;
    void record_failure(std::string stage, std::string error,
                        bool preserve_specific = false) noexcept;
    void record_media_error(std::string error) noexcept;
    bool activate_media(const EncodedAccessUnit* initial_keyframe);
    void enqueue(const EncodedAccessUnit& unit) noexcept;
    void media_loop() noexcept;

    std::string id_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::Track> video_track_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config_;
    std::shared_ptr<rtc::RtcpSrReporter> sender_reporter_;
    BoundedQueue<EncodedAccessUnit> media_queue_;
    std::thread media_worker_;
    std::atomic<bool> media_running_{false};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::chrono::steady_clock::time_point last_activity_;
    std::chrono::steady_clock::time_point connected_at_{};
    std::chrono::steady_clock::time_point connection_wait_started_{};
    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> packets_sent_{0};
    std::atomic<std::uint64_t> packets_retransmitted_{0};
    std::atomic<std::uint64_t> keyframe_events_{0};
    std::atomic<std::uint64_t>* media_errors_;
    TimingSummary media_queue_wait_;
    std::string peer_state_ = "new";
    std::string ice_state_ = "new";
    std::string local_candidate_;
    std::string local_interface_;
    std::string failure_stage_;
    std::string close_reason_;
    std::string last_error_;
    bool local_description_ready_ = false;
    bool gathering_complete_ = false;
    bool connected_ = false;
    bool answer_ready_ = false;
    bool failed_ = false;
    bool closed_ = false;
    bool cleanup_started_ = false;
};

class WebRtcManager {
public:
    explicit WebRtcManager(Logger& logger,
        std::chrono::milliseconds stale_timeout = std::chrono::milliseconds(0),
        std::chrono::milliseconds gathering_timeout = std::chrono::seconds(5));
    virtual ~WebRtcManager();

    WebRtcManager(const WebRtcManager&) = delete;
    WebRtcManager& operator=(const WebRtcManager&) = delete;

    void configure(const WebrtcConfig& config);
    virtual CreateSessionResult create_session(std::string_view offer_sdp);
    bool close_session(std::string_view session_id);
    std::size_t cleanup_stale_sessions();
    std::size_t session_count() const;
    WebRtcDiagnostics diagnostics() const;
    void set_media_available(bool available, std::string reason = {});
    void publish_access_unit(const EncodedAccessUnit& unit) noexcept;
    void shutdown() noexcept;

protected:
    virtual std::shared_ptr<WebRtcSession> create_peer_session(
        std::string id, std::size_t queue_capacity,
        std::atomic<std::uint64_t>* media_errors);

private:
    std::string make_session_id();
    void remember_closed(const std::shared_ptr<WebRtcSession>& session,
                         const std::string& reason) noexcept;
    void cleanup_loop();

    Logger& logger_;
    std::chrono::milliseconds connection_timeout_;
    const bool timeout_overridden_;
    const std::chrono::milliseconds gathering_timeout_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::unordered_map<std::string, std::shared_ptr<WebRtcSession>> sessions_;
    std::deque<WebRtcPeerDiagnostics> recently_closed_;
    std::unique_ptr<EncodedAccessUnit> latest_keyframe_;
    std::thread cleanup_worker_;
    std::size_t max_peers_ = 0;
    std::size_t media_queue_capacity_ = 8;
    std::uint64_t sessions_created_ = 0;
    std::uint64_t sessions_closed_ = 0;
    std::uint64_t signaling_errors_ = 0;
    std::atomic<std::uint64_t> media_errors_{0};
    std::string last_close_reason_;
    std::string media_unavailable_reason_;
    bool enabled_ = false;
    bool media_available_ = true;
    bool stopping_ = true;
};

} // namespace skai
