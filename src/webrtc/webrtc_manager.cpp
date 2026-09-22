#include "skai/webrtc/webrtc_manager.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

namespace skai {
namespace {

bool is_compatible_h264(const rtc::Description::Media::RtpMap* map) {
    if (!map) return false;
    std::string format = map->format;
    std::transform(format.begin(), format.end(), format.begin(),
                   [](unsigned char value) { return std::toupper(value); });
    if (format != "H264") return false;
    for (auto parameters : map->fmtps) {
        std::transform(parameters.begin(), parameters.end(), parameters.begin(),
                       [](unsigned char value) { return std::tolower(value); });
        constexpr std::size_t value_offset = 17;
        const auto profile = parameters.find("profile-level-id=42");
        if (profile == std::string::npos ||
            profile + value_offset + 6 > parameters.size() ||
            parameters.find("packetization-mode=1") == std::string::npos) continue;
        const auto* level_begin = parameters.data() + profile + value_offset + 4;
        unsigned int level = 0;
        const auto parsed = std::from_chars(level_begin, level_begin + 2, level, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == level_begin + 2 && level <= 0x1f) {
            return true;
        }
    }
    return false;
}

std::uint32_t random_ssrc() {
    std::random_device random;
    return (static_cast<std::uint32_t>(random()) << 16) ^
           static_cast<std::uint32_t>(random());
}

} // namespace

WebRtcSession::WebRtcSession(std::string id)
    : id_(std::move(id)), peer_(std::make_shared<rtc::PeerConnection>()),
      last_activity_(std::chrono::steady_clock::now()) {}

std::shared_ptr<WebRtcSession> WebRtcSession::create(std::string id) {
    auto session = std::shared_ptr<WebRtcSession>(new WebRtcSession(std::move(id)));
    const std::weak_ptr<WebRtcSession> weak = session;
    session->peer_->onLocalDescription([weak](rtc::Description) {
        if (const auto current = weak.lock()) {
            std::lock_guard<std::mutex> lock(current->mutex_);
            current->local_description_ready_ = true;
            current->last_activity_ = std::chrono::steady_clock::now();
            current->changed_.notify_all();
        }
    });
    session->peer_->onGatheringStateChange(
        [weak](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete) return;
            if (const auto current = weak.lock()) {
                std::lock_guard<std::mutex> lock(current->mutex_);
                current->gathering_complete_ = true;
                current->last_activity_ = std::chrono::steady_clock::now();
                current->changed_.notify_all();
            }
        });
    session->peer_->onStateChange([weak](rtc::PeerConnection::State state) {
        if (const auto current = weak.lock()) {
            std::lock_guard<std::mutex> lock(current->mutex_);
            if (state == rtc::PeerConnection::State::Connected) {
                current->connected_ = true;
                current->last_activity_ = std::chrono::steady_clock::now();
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed) {
                current->failed_ = true;
            } else if (state == rtc::PeerConnection::State::Closed) {
                current->closed_ = true;
            } else {
                return;
            }
            current->changed_.notify_all();
        }
    });
    return session;
}

WebRtcSession::~WebRtcSession() { close(); }

CreateSessionResult WebRtcSession::accept_offer(
        std::string_view offer, std::chrono::milliseconds timeout) {
    std::shared_ptr<rtc::PeerConnection> peer;
    try {
        rtc::Description description(std::string(offer), rtc::Description::Type::Offer);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || !peer_) {
                return {CreateSessionError::Internal, {}, {},
                        "PeerConnection closed while accepting the offer"};
            }
            peer = peer_;
        }
        auto session_direction = rtc::Description::Direction::SendRecv;
        for (const auto& attribute : description.attributes()) {
            if (attribute == "sendonly") session_direction = rtc::Description::Direction::SendOnly;
            if (attribute == "recvonly") session_direction = rtc::Description::Direction::RecvOnly;
            if (attribute == "inactive") session_direction = rtc::Description::Direction::Inactive;
        }
        bool video_added = false;
        for (int index = 0; index < description.mediaCount(); ++index) {
            const auto entry = description.media(index);
            const auto media = std::get_if<rtc::Description::Media*>(&entry);
            if (!media || (*media)->isRemoved()) continue;
            const auto direction = (*media)->direction() == rtc::Description::Direction::Unknown
                ? session_direction : (*media)->direction();
            if (direction == rtc::Description::Direction::SendOnly ||
                direction == rtc::Description::Direction::Inactive) {
                return {CreateSessionError::InvalidOffer, {}, {},
                        "WHEP media must be recvonly or sendrecv"};
            }
            int h264_payload_type = -1;
            for (const int payload_type : (*media)->payloadTypes()) {
                if (is_compatible_h264((*media)->rtpMap(payload_type))) {
                    h264_payload_type = payload_type;
                    break;
                }
            }
            if (h264_payload_type < 0 || video_added) continue;

            auto answer_media = (*media)->reciprocate();
            answer_media.setDirection(rtc::Description::Direction::SendOnly);
            for (const int payload_type : answer_media.payloadTypes()) {
                if (payload_type != h264_payload_type) answer_media.removeRtpMap(payload_type);
            }
            const auto ssrc = random_ssrc();
            answer_media.addSSRC(ssrc, "skai-edge", "skai-edge", "video");
            video_track_ = peer->addTrack(std::move(answer_media));
            rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, "skai-edge", static_cast<std::uint8_t>(h264_payload_type),
                rtc::H264RtpPacketizer::defaultClockRate);
            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, rtp_config_);
            sender_reporter_ = std::make_shared<rtc::RtcpSrReporter>(rtp_config_);
            packetizer->addToChain(sender_reporter_);
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
            video_track_->setMediaHandler(packetizer);
            video_added = true;
        }
        if (!video_added) {
            return {CreateSessionError::InvalidOffer, {}, {},
                    "WHEP offer must contain recvonly H.264 video"};
        }
        peer->setRemoteDescription(std::move(description));
    } catch (const std::invalid_argument& error) {
        return {CreateSessionError::InvalidOffer, {}, {}, error.what()};
    } catch (const std::exception& error) {
        return {CreateSessionError::Internal, {}, {}, error.what()};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = changed_.wait_for(lock, timeout, [this] {
        return (local_description_ready_ && gathering_complete_) || failed_ || closed_;
    });
    if (!ready) {
        lock.unlock();
        close();
        return {CreateSessionError::GatheringTimeout, {}, {},
                "ICE gathering timed out"};
    }
    if (failed_ || closed_) {
        return {CreateSessionError::Internal, {}, {},
                "PeerConnection closed while creating the answer"};
    }
    last_activity_ = std::chrono::steady_clock::now();
    lock.unlock();

    const auto local = peer->localDescription();
    if (!local || local->type() != rtc::Description::Type::Answer) {
        return {CreateSessionError::Internal, {}, {},
                "PeerConnection did not produce an SDP answer"};
    }
    lock.lock();
    if (closed_ || failed_) {
        return {CreateSessionError::Internal, {}, {},
                "PeerConnection closed while creating the answer"};
    }
    return {CreateSessionError::None, id_, std::string(*local), {}};
}

void WebRtcSession::close() noexcept {
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cleanup_started_) return;
        cleanup_started_ = true;
        closed_ = true;
        media_running_ = false;
        peer = std::move(peer_);
    }
    changed_.notify_all();
    media_queue_.shutdown();
    if (media_worker_.joinable()) media_worker_.join();
    if (!peer) return;
    try {
        peer->resetCallbacks();
        peer->close();
    } catch (...) {
    }
}

bool WebRtcSession::activate_media(const EncodedAccessUnit* initial_keyframe) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || cleanup_started_) return false;
    media_queue_.reset();
    if (initial_keyframe) media_queue_.push(*initial_keyframe);
    media_running_ = true;
    try {
        media_worker_ = std::thread(&WebRtcSession::media_loop, this);
    } catch (...) {
        media_running_ = false;
        media_queue_.shutdown();
        return false;
    }
    return true;
}

void WebRtcSession::enqueue(const EncodedAccessUnit& unit) noexcept {
    if (!media_running_) return;
    try {
        media_queue_.push(unit);
    } catch (...) {
    }
}

void WebRtcSession::media_loop() noexcept {
    bool waiting_for_keyframe = true;
    bool timestamp_initialized = false;
    std::uint64_t last_pts_ns = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint32_t last_timestamp_step = 3'000;
    std::size_t observed_queue_drops = 0;
    while (media_running_) {
        auto unit = media_queue_.pop_for(std::chrono::milliseconds(100));
        if (!unit) continue;
        const auto queue_drops = media_queue_.stats().dropped;
        if (queue_drops != observed_queue_drops) {
            observed_queue_drops = queue_drops;
            waiting_for_keyframe = true;
        }
        if (waiting_for_keyframe && !unit->keyframe) continue;
        std::shared_ptr<rtc::Track> track;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            track = video_track_;
        }
        while (media_running_ && track && !track->isOpen()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!media_running_ || !track || !track->isOpen()) continue;
        if (waiting_for_keyframe) {
            waiting_for_keyframe = false;
        }
        if (!timestamp_initialized) {
            timestamp_initialized = true;
            rtp_timestamp = rtp_config_->startTimestamp;
        } else {
            std::uint32_t step = last_timestamp_step;
            if (unit->pts_ns > last_pts_ns) {
                const auto delta_ns = unit->pts_ns - last_pts_ns;
                const auto converted =
                    (delta_ns / 1'000'000'000ULL) * 90'000ULL +
                    ((delta_ns % 1'000'000'000ULL) * 90'000ULL) / 1'000'000'000ULL;
                step = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, converted));
                if (step <= 90'000) last_timestamp_step = step;
            }
            rtp_timestamp += step;
        }
        last_pts_ns = unit->pts_ns;
        rtp_config_->timestamp = rtp_timestamp;
        if (sender_reporter_ &&
            rtp_config_->timestampToSeconds(
                rtp_config_->timestamp - sender_reporter_->lastReportedTimestamp()) > 1.0) {
            sender_reporter_->setNeedsToReport();
        }
        try {
            track->send(reinterpret_cast<const std::byte*>(unit->bytes.data()),
                        unit->bytes.size());
        } catch (...) {
            media_running_ = false;
        }
    }
}

bool WebRtcSession::stale(std::chrono::steady_clock::time_point now,
                          std::chrono::milliseconds timeout) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_ || failed_ || (!connected_ && now - last_activity_ >= timeout);
}

WebRtcManager::WebRtcManager(Logger& logger,
        std::chrono::milliseconds stale_timeout,
        std::chrono::milliseconds gathering_timeout)
    : logger_(logger), stale_timeout_(stale_timeout),
      gathering_timeout_(gathering_timeout) {}

WebRtcManager::~WebRtcManager() { shutdown(); }

void WebRtcManager::configure(const WebrtcConfig& config) {
    shutdown();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = config.enabled;
        max_peers_ = static_cast<std::size_t>(config.max_peers);
        stopping_ = !enabled_;
    }
    if (config.enabled) cleanup_worker_ = std::thread([this] { cleanup_loop(); });
}

CreateSessionResult WebRtcManager::create_session(std::string_view offer_sdp) {
    if (offer_sdp.empty()) {
        return {CreateSessionError::InvalidOffer, {}, {}, "SDP offer is empty"};
    }
    cleanup_stale_sessions();
    std::shared_ptr<WebRtcSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || stopping_) {
            return {CreateSessionError::Disabled, {}, {}, "WebRTC is disabled"};
        }
        if (sessions_.size() >= max_peers_) {
            return {CreateSessionError::Capacity, {}, {}, "maximum peer count reached"};
        }
        std::string id;
        do id = make_session_id(); while (sessions_.count(id) != 0);
        session = WebRtcSession::create(id);
        sessions_.emplace(id, session);
    }

    auto result = session->accept_offer(offer_sdp, gathering_timeout_);
    if (!result) {
        close_session(session->id());
        return result;
    }
    std::unique_ptr<EncodedAccessUnit> initial_keyframe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(session->id());
        if (stopping_ || found == sessions_.end() || found->second != session) {
            session->close();
            return {CreateSessionError::Disabled, {}, {}, "WebRTC is shutting down"};
        }
        if (latest_keyframe_) {
            initial_keyframe = std::make_unique<EncodedAccessUnit>(*latest_keyframe_);
        }
    }
    if (!session->activate_media(initial_keyframe.get())) {
        close_session(session->id());
        return {CreateSessionError::Disabled, {}, {}, "WebRTC is shutting down"};
    }
    logger_.log(LogLevel::Info, "webrtc", "created WHEP session " + result.session_id);
    return result;
}

bool WebRtcManager::close_session(std::string_view session_id) {
    std::shared_ptr<WebRtcSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(std::string(session_id));
        if (found == sessions_.end()) return false;
        session = std::move(found->second);
        sessions_.erase(found);
    }
    session->close();
    logger_.log(LogLevel::Info, "webrtc", "closed WHEP session " + session->id());
    return true;
}

std::size_t WebRtcManager::cleanup_stale_sessions() {
    std::vector<std::shared_ptr<WebRtcSession>> stale;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto item = sessions_.begin(); item != sessions_.end();) {
            if (!item->second->stale(now, stale_timeout_)) {
                ++item;
                continue;
            }
            stale.push_back(std::move(item->second));
            item = sessions_.erase(item);
        }
    }
    for (const auto& session : stale) session->close();
    if (!stale.empty()) {
        logger_.log(LogLevel::Info, "webrtc",
                    "cleaned " + std::to_string(stale.size()) + " stale session(s)");
    }
    return stale.size();
}

std::size_t WebRtcManager::session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void WebRtcManager::publish_access_unit(const EncodedAccessUnit& unit) noexcept {
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || stopping_) return;
        if (unit.keyframe) latest_keyframe_ = std::make_unique<EncodedAccessUnit>(unit);
        sessions.reserve(sessions_.size());
        for (const auto& item : sessions_) sessions.push_back(item.second);
    } catch (...) {
        return;
    }
    for (const auto& session : sessions) session->enqueue(unit);
}

void WebRtcManager::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = false;
        stopping_ = true;
    }
    wakeup_.notify_all();
    if (cleanup_worker_.joinable()) cleanup_worker_.join();
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : sessions_) sessions.push_back(std::move(item.second));
        sessions_.clear();
        latest_keyframe_.reset();
    }
    for (const auto& session : sessions) session->close();
}

std::string WebRtcManager::make_session_id() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes) output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

void WebRtcManager::cleanup_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto interval = std::min(std::chrono::milliseconds(1000),
        std::max(std::chrono::milliseconds(10), stale_timeout_ / 2));
    while (!stopping_) {
        if (wakeup_.wait_for(lock, interval, [this] { return stopping_; })) break;
        lock.unlock();
        cleanup_stale_sessions();
        lock.lock();
    }
}

} // namespace skai
