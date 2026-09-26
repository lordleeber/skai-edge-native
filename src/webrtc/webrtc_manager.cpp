#include "skai/webrtc/webrtc_manager.hpp"

#include <rtc/rtc.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtp.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_map>
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

template <typename State>
std::string state_name(State state) {
    std::ostringstream output;
    output << state;
    return output.str();
}

std::string interface_for_address(const std::string& address) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return {};
    std::string result;
    for (auto* item = interfaces; item && result.empty(); item = item->ifa_next) {
        if (!item->ifa_addr) continue;
        char text[INET6_ADDRSTRLEN]{};
        const void* source = nullptr;
        if (item->ifa_addr->sa_family == AF_INET) {
            source = &reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr;
        } else if (item->ifa_addr->sa_family == AF_INET6) {
            source = &reinterpret_cast<sockaddr_in6*>(item->ifa_addr)->sin6_addr;
        }
        if (source && inet_ntop(item->ifa_addr->sa_family, source, text, sizeof(text)) &&
            address == text) result = item->ifa_name;
    }
    freeifaddrs(interfaces);
    return result;
}

std::string endpoint_address(const std::string& endpoint) {
    if (endpoint.empty()) return {};
    if (endpoint.front() == '[') {
        const auto close = endpoint.find(']');
        return close == std::string::npos ? std::string{} : endpoint.substr(1, close - 1);
    }
    const auto colon = endpoint.rfind(':');
    return colon == std::string::npos ? endpoint : endpoint.substr(0, colon);
}

class CountingNackResponder final : public rtc::MediaHandler {
public:
    CountingNackResponder(std::atomic<std::uint64_t>& bytes,
                          std::atomic<std::uint64_t>& packets,
                          std::atomic<std::uint64_t>& retransmissions)
        : bytes_(bytes), packet_count_(packets), retransmissions_(retransmissions) {}

    void outgoing(rtc::message_vector& messages,
                  const rtc::message_callback&) override {
        std::uint64_t bytes = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& message : messages) {
            bytes += message->size();
            if (message->type == rtc::Message::Control ||
                message->size() < sizeof(rtc::RtpHeader)) continue;
            const auto sequence =
                reinterpret_cast<const rtc::RtpHeader*>(message->data())->seqNumber();
            if (packets_.find(sequence) == packets_.end()) order_.push_back(sequence);
            packets_[sequence] = message;
            while (order_.size() > 512) {
                packets_.erase(order_.front());
                order_.pop_front();
            }
        }
        bytes_.fetch_add(bytes, std::memory_order_relaxed);
        packet_count_.fetch_add(messages.size(), std::memory_order_relaxed);
    }

    void incoming(rtc::message_vector& messages,
                  const rtc::message_callback& send) override {
        for (const auto& message : messages) {
            if (message->type != rtc::Message::Control) continue;
            std::size_t offset = 0;
            while (offset + sizeof(rtc::RtcpNack) <= message->size()) {
                auto* nack = reinterpret_cast<rtc::RtcpNack*>(
                    message->data() + offset);
                const auto length = nack->header.header.lengthInBytes();
                if (length == 0 || offset + length > message->size()) break;
                offset += length;
                if (nack->header.header.payloadType() != 205 ||
                    nack->header.header.reportCount() != 1) continue;
                for (unsigned int index = 0; index < nack->getSeqNoCount(); ++index) {
                    for (const auto sequence : nack->parts[index].getSequenceNumbers()) {
                        rtc::message_ptr retransmission;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            const auto found = packets_.find(sequence);
                            if (found != packets_.end()) {
                                retransmission = rtc::make_message(*found->second);
                            }
                        }
                        if (!retransmission) continue;
                        bytes_.fetch_add(retransmission->size(), std::memory_order_relaxed);
                        packet_count_.fetch_add(1, std::memory_order_relaxed);
                        retransmissions_.fetch_add(1, std::memory_order_relaxed);
                        send(std::move(retransmission));
                    }
                }
            }
        }
    }

private:
    std::atomic<std::uint64_t>& bytes_;
    std::atomic<std::uint64_t>& packet_count_;
    std::atomic<std::uint64_t>& retransmissions_;
    std::mutex mutex_;
    std::deque<std::uint16_t> order_;
    std::unordered_map<std::uint16_t, rtc::message_ptr> packets_;
};

} // namespace

WebRtcSession::WebRtcSession(std::string id, std::size_t queue_capacity,
                             std::atomic<std::uint64_t>* media_errors)
    : id_(std::move(id)), peer_(std::make_shared<rtc::PeerConnection>()),
      media_queue_(queue_capacity), last_activity_(std::chrono::steady_clock::now()),
      media_errors_(media_errors) {}

std::shared_ptr<WebRtcSession> WebRtcSession::create(
        std::string id, std::size_t queue_capacity,
        std::atomic<std::uint64_t>* media_errors) {
    auto session = std::shared_ptr<WebRtcSession>(
        new WebRtcSession(std::move(id), queue_capacity, media_errors));
    const std::weak_ptr<WebRtcSession> weak = session;
    session->peer_->onLocalDescription([weak](rtc::Description) {
        if (const auto current = weak.lock()) {
            std::lock_guard<std::mutex> lock(current->mutex_);
            current->local_description_ready_ = true;
            current->last_activity_ = std::chrono::steady_clock::now();
            current->changed_.notify_all();
        }
    });
    session->peer_->onLocalCandidate([weak](rtc::Candidate candidate) {
        if (const auto current = weak.lock()) {
            candidate.resolve(rtc::Candidate::ResolveMode::Simple);
            std::lock_guard<std::mutex> lock(current->mutex_);
            current->local_candidate_ = std::string(candidate);
            if (const auto address = candidate.address()) {
                current->local_interface_ = interface_for_address(*address);
            }
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
            current->peer_state_ = state_name(state);
            if (state == rtc::PeerConnection::State::Connected) {
                current->connected_ = true;
                current->connected_at_ = std::chrono::steady_clock::now();
                current->last_activity_ = std::chrono::steady_clock::now();
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed) {
                current->failed_ = true;
                if (current->failure_stage_.empty()) {
                    current->failure_stage_ = "peer_connection";
                    current->last_error_ = state == rtc::PeerConnection::State::Failed
                        ? "PeerConnection failed" : "PeerConnection disconnected";
                }
            } else if (state == rtc::PeerConnection::State::Closed) {
                current->closed_ = true;
            } else {
                return;
            }
            current->changed_.notify_all();
        }
    });
    session->peer_->onIceStateChange([weak](rtc::PeerConnection::IceState state) {
        if (const auto current = weak.lock()) {
            std::lock_guard<std::mutex> lock(current->mutex_);
            current->ice_state_ = state_name(state);
            if (state == rtc::PeerConnection::IceState::Failed ||
                state == rtc::PeerConnection::IceState::Disconnected) {
                current->failed_ = true;
                if (current->failure_stage_.empty() ||
                    current->failure_stage_ == "peer_connection") {
                    current->failure_stage_ = "ice";
                    current->last_error_ = state == rtc::PeerConnection::IceState::Failed
                        ? "ICE connectivity failed" : "ICE disconnected";
                }
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
            packetizer->addToChain(
                std::make_shared<CountingNackResponder>(
                    bytes_sent_, packets_sent_, packets_retransmitted_));
            packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak = weak_from_this()] {
                if (const auto current = weak.lock()) {
                    current->keyframe_events_.fetch_add(1, std::memory_order_relaxed);
                }
            }));
            video_track_->setMediaHandler(packetizer);
            video_track_->onError([weak = weak_from_this()](std::string error) {
                if (const auto current = weak.lock()) {
                    current->record_media_error(std::move(error));
                }
            });
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
        return {CreateSessionError::GatheringTimeout, {}, {},
                "ICE gathering timed out"};
    }
    if (failed_ || closed_) {
        return {CreateSessionError::Internal, {}, {},
                "PeerConnection closed while creating the answer"};
    }
    last_activity_ = std::chrono::steady_clock::now();
    lock.unlock();

    try {
        const auto local = peer->localDescription();
        if (!local || local->type() != rtc::Description::Type::Answer) {
            return {CreateSessionError::Internal, {}, {},
                    "PeerConnection did not produce an SDP answer"};
        }
        const auto answer = std::string(*local);
        lock.lock();
        if (closed_ || failed_) {
            return {CreateSessionError::Internal, {}, {},
                    "PeerConnection closed while creating the answer"};
        }
        answer_ready_ = true;
        connection_wait_started_ = std::chrono::steady_clock::now();
        return {CreateSessionError::None, id_, answer, {}};
    } catch (const std::exception& error) {
        return {CreateSessionError::Internal, {}, {}, error.what()};
    }
}

void WebRtcSession::close() noexcept { close_with_reason("session_closed"); }

void WebRtcSession::close_with_reason(std::string reason) noexcept {
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cleanup_started_) return;
        cleanup_started_ = true;
        closed_ = true;
        peer_state_ = "closed";
        if (close_reason_.empty()) close_reason_ = std::move(reason);
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
    if (initial_keyframe) {
        auto queued = *initial_keyframe;
        queued.queued_at = std::chrono::steady_clock::now();
        media_queue_.push(std::move(queued));
    }
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
        auto queued = unit;
        queued.queued_at = std::chrono::steady_clock::now();
        media_queue_.push(std::move(queued));
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
        if (unit->queued_at != std::chrono::steady_clock::time_point{}) {
            const auto wait_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - unit->queued_at).count();
            std::lock_guard<std::mutex> lock(mutex_);
            media_queue_wait_.observe(wait_ms);
        }
        if (unit->discontinuity) {
            waiting_for_keyframe = true;
            timestamp_initialized = false;
        }
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
        if (unit->has_pts) {
            timestamp_initialized = true;
            rtp_timestamp = h264_rtp_timestamp(unit->pts_ns);
        } else if (!timestamp_initialized) {
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
            if (!track->send(reinterpret_cast<const std::byte*>(unit->bytes.data()),
                             unit->bytes.size())) {
                record_media_error("media track rejected an access unit");
                media_running_ = false;
            }
        } catch (const std::exception& error) {
            record_media_error(error.what());
            media_running_ = false;
        } catch (...) {
            record_media_error("unknown media delivery error");
            media_running_ = false;
        }
    }
}

void WebRtcSession::record_media_error(std::string error) noexcept {
    record_failure("media", std::move(error));
    if (media_errors_) media_errors_->fetch_add(1, std::memory_order_relaxed);
}

void WebRtcSession::record_failure(std::string stage, std::string error,
                                   bool preserve_specific) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (preserve_specific && !failure_stage_.empty()) return;
        failure_stage_ = std::move(stage);
        last_error_ = std::move(error);
        failed_ = true;
    } catch (...) {
    }
}

WebRtcPeerDiagnostics WebRtcSession::diagnostics() const {
    WebRtcPeerDiagnostics result;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::chrono::steady_clock::time_point connected_at;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.session_id = id_;
        result.media_queue_wait = media_queue_wait_;
        result.peer_state = peer_state_;
        result.ice_state = ice_state_;
        result.local_candidate = local_candidate_;
        result.local_interface = local_interface_;
        result.failure_stage = failure_stage_;
        result.close_reason = close_reason_;
        result.last_error = last_error_;
        connected_at = connected_at_;
        peer = peer_;
    }
    result.bytes_sent = bytes_sent_.load(std::memory_order_relaxed);
    result.packets_sent = packets_sent_.load(std::memory_order_relaxed);
    result.packets_retransmitted =
        packets_retransmitted_.load(std::memory_order_relaxed);
    result.media_queue_drops = media_queue_.stats().dropped;
    result.keyframe_events = keyframe_events_.load(std::memory_order_relaxed);
    if (connected_at != std::chrono::steady_clock::time_point{}) {
        result.connection_age_s = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - connected_at).count());
    }
    try {
        rtc::Candidate local;
        rtc::Candidate remote;
        if (peer && peer->getSelectedCandidatePair(&local, &remote)) {
            result.local_candidate = std::string(local);
            if (const auto address = local.address()) {
                result.selected_interface = interface_for_address(*address);
            }
        } else if (peer) {
            if (const auto local_address = peer->localAddress()) {
                if (result.local_candidate.empty()) result.local_candidate = *local_address;
                result.selected_interface =
                    interface_for_address(endpoint_address(*local_address));
            }
        }
    } catch (...) {
    }
    return result;
}

std::string WebRtcSession::stale_reason(
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds timeout) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) return failure_stage_.empty() ? "peer_failure"
                                               : failure_stage_ + "_failure";
    if (closed_) return close_reason_.empty() ? "peer_closed" : close_reason_;
    if (answer_ready_ && !connected_ && now - connection_wait_started_ >= timeout) {
        return "connection_timeout";
    }
    return {};
}

WebRtcManager::WebRtcManager(Logger& logger,
        std::chrono::milliseconds stale_timeout,
        std::chrono::milliseconds gathering_timeout)
    : logger_(logger),
      connection_timeout_(stale_timeout.count() > 0 ? stale_timeout
                                                     : std::chrono::seconds(15)),
      timeout_overridden_(stale_timeout.count() > 0),
      gathering_timeout_(gathering_timeout) {}

WebRtcManager::~WebRtcManager() { shutdown(); }

std::shared_ptr<WebRtcSession> WebRtcManager::create_peer_session(
        std::string id, std::size_t queue_capacity,
        std::atomic<std::uint64_t>* media_errors) {
    return WebRtcSession::create(std::move(id), queue_capacity, media_errors);
}

void WebRtcManager::configure(const WebrtcConfig& config) {
    shutdown();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = config.enabled;
        max_peers_ = static_cast<std::size_t>(config.max_peers);
        media_queue_capacity_ = static_cast<std::size_t>(config.media_queue_capacity);
        if (!timeout_overridden_) {
            connection_timeout_ =
                std::chrono::milliseconds(config.connection_timeout_ms);
        }
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
        if (!media_available_) {
            return {CreateSessionError::Disabled, {}, {},
                    media_unavailable_reason_.empty()
                        ? "browser-compatible source H.264 is unavailable"
                        : media_unavailable_reason_};
        }
        if (sessions_.size() >= max_peers_) {
            return {CreateSessionError::Capacity, {}, {}, "maximum peer count reached"};
        }
        std::string id;
        do id = make_session_id(); while (sessions_.count(id) != 0);
        try {
            session = create_peer_session(id, media_queue_capacity_, &media_errors_);
        } catch (const std::exception& error) {
            ++signaling_errors_;
            logger_.log(LogLevel::Error, "webrtc",
                        std::string("PeerConnection creation failed: ") + error.what());
            return {CreateSessionError::Internal, {}, {},
                    "PeerConnection creation failed"};
        } catch (...) {
            ++signaling_errors_;
            logger_.log(LogLevel::Error, "webrtc",
                        "PeerConnection creation failed: unknown error");
            return {CreateSessionError::Internal, {}, {},
                    "PeerConnection creation failed"};
        }
        if (!session) {
            ++signaling_errors_;
            return {CreateSessionError::Internal, {}, {},
                    "PeerConnection creation failed"};
        }
        sessions_.emplace(id, session);
        ++sessions_created_;
    }

    auto result = session->accept_offer(offer_sdp, gathering_timeout_);
    if (!result) {
        std::shared_ptr<WebRtcSession> failed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++signaling_errors_;
            const auto found = sessions_.find(session->id());
            if (found != sessions_.end()) {
                failed = std::move(found->second);
                sessions_.erase(found);
                ++sessions_closed_;
                last_close_reason_ = "signaling_failure";
            }
        }
        if (failed) {
            failed->record_failure("signaling", result.message, true);
            remember_closed(failed, "signaling_failure");
        }
        return result;
    }
    std::unique_ptr<EncodedAccessUnit> initial_keyframe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(session->id());
        if (stopping_ || !media_available_ || found == sessions_.end() ||
            found->second != session) {
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
        ++sessions_closed_;
        last_close_reason_ = "client_delete";
    }
    remember_closed(session, "client_delete");
    logger_.log(LogLevel::Info, "webrtc", "closed WHEP session " + session->id());
    return true;
}

std::size_t WebRtcManager::cleanup_stale_sessions() {
    std::vector<std::pair<std::shared_ptr<WebRtcSession>, std::string>> stale;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto item = sessions_.begin(); item != sessions_.end();) {
            const auto reason = item->second->stale_reason(now, connection_timeout_);
            if (reason.empty()) {
                ++item;
                continue;
            }
            stale.emplace_back(std::move(item->second), reason);
            item = sessions_.erase(item);
            ++sessions_closed_;
            last_close_reason_ = reason;
        }
    }
    for (const auto& entry : stale) {
        remember_closed(entry.first, entry.second);
    }
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

WebRtcDiagnostics WebRtcManager::diagnostics() const {
    WebRtcDiagnostics result;
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.enabled = enabled_ && !stopping_;
        result.media_available = media_available_;
        result.media_unavailable_reason = media_unavailable_reason_;
        result.keyframe_cached = static_cast<bool>(latest_keyframe_);
        result.max_peers = max_peers_;
        result.sessions_created = sessions_created_;
        result.sessions_closed = sessions_closed_;
        result.signaling_errors = signaling_errors_;
        result.media_errors = media_errors_.load(std::memory_order_relaxed);
        result.last_close_reason = last_close_reason_;
        result.recently_closed.assign(recently_closed_.begin(), recently_closed_.end());
        sessions.reserve(sessions_.size());
        for (const auto& item : sessions_) sessions.push_back(item.second);
    }
    result.peers.reserve(sessions.size());
    for (const auto& session : sessions) result.peers.push_back(session->diagnostics());
    return result;
}

void WebRtcManager::publish_access_unit(const EncodedAccessUnit& unit) noexcept {
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || stopping_ || !media_available_) return;
        if (unit.discontinuity) latest_keyframe_.reset();
        if (unit.keyframe) latest_keyframe_ = std::make_unique<EncodedAccessUnit>(unit);
        sessions.reserve(sessions_.size());
        for (const auto& item : sessions_) sessions.push_back(item.second);
    } catch (...) {
        return;
    }
    for (const auto& session : sessions) session->enqueue(unit);
}

void WebRtcManager::set_media_available(bool available, std::string reason) {
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (media_available_ == available &&
            media_unavailable_reason_ == (available ? std::string{} : reason)) return;
        media_available_ = available;
        media_unavailable_reason_ = available ? std::string{} : std::move(reason);
        if (available) return;
        latest_keyframe_.reset();
        for (auto& item : sessions_) sessions.push_back(std::move(item.second));
        if (!sessions.empty()) {
            sessions_closed_ += sessions.size();
            last_close_reason_ = "source_media_unavailable";
        }
        sessions_.clear();
    }
    for (const auto& session : sessions) {
        remember_closed(session, "source_media_unavailable");
    }
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
        if (!sessions.empty()) {
            sessions_closed_ += sessions.size();
            last_close_reason_ = "service_shutdown";
        }
        sessions_.clear();
        latest_keyframe_.reset();
    }
    for (const auto& session : sessions) remember_closed(session, "service_shutdown");
}

void WebRtcManager::remember_closed(
        const std::shared_ptr<WebRtcSession>& session,
        const std::string& reason) noexcept {
    if (!session) return;
    session->close_with_reason(reason);
    try {
        auto snapshot = session->diagnostics();
        std::lock_guard<std::mutex> lock(mutex_);
        recently_closed_.push_back(std::move(snapshot));
        while (recently_closed_.size() > 16) recently_closed_.pop_front();
    } catch (...) {
    }
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
        std::max(std::chrono::milliseconds(10), connection_timeout_ / 2));
    while (!stopping_) {
        if (wakeup_.wait_for(lock, interval, [this] { return stopping_; })) break;
        lock.unlock();
        cleanup_stale_sessions();
        lock.lock();
    }
}

} // namespace skai
