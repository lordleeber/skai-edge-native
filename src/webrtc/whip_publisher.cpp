#include "skai/webrtc/whip_publisher.hpp"
#include "webrtc/whip_policy.hpp"

#include <rtc/rtc.hpp>
#include <rtc/rtp.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace skai {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string load_token() {
    if (const char* value = std::getenv("WHIP_TOKEN")) {
        if (*value) return value;
    }
    std::ifstream input(".env");
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));
        if (line.rfind("WHIP_TOKEN=", 0) != 0) continue;
        auto value = trim(line.substr(11));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string location;
};

size_t receive_body(char* data, size_t size, size_t count, void* user) {
    auto& body = *static_cast<std::string*>(user);
    const auto length = size * count;
    if (body.size() + length > 128 * 1024) return 0;
    body.append(data, length);
    return length;
}

size_t receive_header(char* data, size_t size, size_t count, void* user) {
    auto& response = *static_cast<HttpResponse*>(user);
    const auto length = size * count;
    std::string header(data, length);
    const auto separator = header.find(':');
    if (separator != std::string::npos) {
        std::string name = header.substr(0, separator);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name == "location") response.location = trim(header.substr(separator + 1));
    }
    return length;
}

int cancel_if_stopping(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
}

HttpResponse request(const std::string& url, const std::string& method,
                     const std::string& body, const std::string& token,
                     std::atomic<bool>& stopping) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                             curl_easy_cleanup);
    if (!curl) throw std::runtime_error("could not initialize WHIP HTTP client");
    HttpResponse response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    const long timeout_ms = method == "DELETE" ? 2000L : 10000L;
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                     std::min(5000L, timeout_ms));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, cancel_if_stopping);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &stopping);
    struct curl_slist* raw_headers = nullptr;
    if (method == "POST" || method == "DELETE") {
        raw_headers = curl_slist_append(raw_headers,
                                        ("Authorization: Bearer " + token).c_str());
    }
    if (method == "POST") {
        raw_headers = curl_slist_append(raw_headers, "Content-Type: application/sdp");
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers,
                                                                        curl_slist_free_all);
    if (headers) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string("WHIP HTTP request failed: ") +
                                 curl_easy_strerror(result));
    }
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

std::uint32_t random_ssrc() {
    std::random_device random;
    return (static_cast<std::uint32_t>(random()) << 16) ^ random();
}

} // namespace

bool WhipPublisher::initialize(const Config& config) {
    config_ = config.whip;
    token_.clear();
    last_error_.clear();
    if (!config_.enabled) return true;
    token_ = load_token();
    if (token_.empty()) {
        last_error_ = "WHIP_TOKEN is required when whip.enabled is true";
        return false;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        last_error_ = "could not initialize WHIP HTTP client";
        return false;
    }
    media_queue_.reset();
    stopping_ = false;
    return true;
}

bool WhipPublisher::start() {
    if (!config_.enabled) return true;
    try {
        worker_ = std::thread(&WhipPublisher::run, this);
        return true;
    } catch (const std::exception& error) {
        last_error_ = error.what();
        return false;
    }
}

void WhipPublisher::stop() noexcept {
    stopping_ = true;
    media_queue_.shutdown();
    wait_changed_.notify_all();
}

void WhipPublisher::wait() noexcept {
    if (worker_.joinable()) worker_.join();
    token_.clear();
}

void WhipPublisher::publish_access_unit(const EncodedAccessUnit& unit) noexcept {
    if (!config_.enabled || stopping_) return;
    try { media_queue_.push(unit); } catch (...) {}
}

void WhipPublisher::run() noexcept {
    while (!stopping_) {
        try {
            publish_once();
        } catch (const UnrecoverableWhipError& error) {
            if (!stopping_) logger_.log(LogLevel::Error, "whip", error.what());
            return;
        } catch (const std::exception& error) {
            if (!stopping_) logger_.log(LogLevel::Error, "whip", error.what());
        } catch (...) {
            if (!stopping_) logger_.log(LogLevel::Error, "whip", "unknown publisher error");
        }
        if (stopping_) break;
        media_queue_.discard_all();
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_changed_.wait_for(lock, std::chrono::seconds(3), [this] { return stopping_.load(); });
    }
}

void WhipPublisher::publish_once() {
    token_ = load_token();
    if (token_.empty()) throw std::runtime_error("WHIP_TOKEN is unavailable");
    auto peer = std::make_shared<rtc::PeerConnection>();
    auto state = std::make_shared<WhipPeerState>();
    const std::weak_ptr<WhipPeerState> weak = state;
    peer->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState value) {
        if (value != rtc::PeerConnection::GatheringState::Complete) return;
        if (auto state = weak.lock()) {
            state->gathered();
        }
    });
    peer->onStateChange([weak](rtc::PeerConnection::State value) {
        if (auto state = weak.lock()) {
            if (value == rtc::PeerConnection::State::Connected) state->connected();
            else if (value == rtc::PeerConnection::State::Disconnected) {
                state->disconnected(WhipPeerState::Clock::now());
            } else if (value == rtc::PeerConnection::State::Failed ||
                       value == rtc::PeerConnection::State::Closed) state->failed();
        }
    });
    const auto ssrc = random_ssrc();
    auto video = rtc::Description::Video("video");
    video.addH264Codec(96);
    video.addSSRC(ssrc, "skai-edge", "skai-edge", "video");
    video.setDirection(rtc::Description::Direction::SendOnly);
    auto track = peer->addTrack(video);
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, "skai-edge", 96, rtc::H264RtpPacketizer::defaultClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, rtp);
    auto reporter = std::make_shared<rtc::RtcpSrReporter>(rtp);
    packetizer->addToChain(reporter);
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    track->setMediaHandler(packetizer);
    std::string location;
    std::uint64_t sent_units = 0;
    const auto close_resource = [this, &location, &sent_units] {
        if (location.empty()) return;
        std::atomic<bool> allow_cleanup{false};
        try {
            const auto current_token = load_token();
            const auto response = request(location, "DELETE", {},
                                          current_token.empty() ? token_ : current_token,
                                          allow_cleanup);
            if (response.status < 200 || response.status >= 300) {
                logger_.log(LogLevel::Warning, "whip", "DELETE returned HTTP " +
                            std::to_string(response.status));
            } else {
                logger_.log(LogLevel::Info, "whip", "publisher session closed; access units sent=" +
                            std::to_string(sent_units));
            }
        } catch (const std::exception& error) {
            logger_.log(LogLevel::Warning, "whip", error.what());
        }
    };
    try {
        peer->setLocalDescription(rtc::Description::Type::Offer);
        if (!state->wait_for_gathering(stopping_, std::chrono::seconds(5))) {
            if (stopping_) return;
            throw std::runtime_error("WHIP ICE gathering did not complete");
        }
        if (stopping_) return;
        const auto offer = peer->localDescription();
        if (!offer) throw std::runtime_error("WHIP SDP offer was not generated");
        const auto response = request(config_.url, "POST", std::string(*offer), token_, stopping_);
        if (response.status != 201) {
            throw std::runtime_error("WHIP POST returned HTTP " +
                                     std::to_string(response.status));
        }
        location = whip_resource_url(config_.url, response.location);
        peer->setRemoteDescription(rtc::Description(response.body,
                                                    rtc::Description::Type::Answer));
        logger_.log(LogLevel::Info, "whip", "publisher session created");
        bool waiting_for_keyframe = true;
        bool timestamp_ready = false;
        bool media_opened = false;
        bool awaiting_resume = false;
        std::optional<std::chrono::steady_clock::time_point> track_closed_at;
        std::size_t observed_drops = media_queue_.stats().dropped;
        const auto connection_deadline = std::chrono::steady_clock::now() +
                                         std::chrono::seconds(15);
        while (!stopping_) {
            const auto now = std::chrono::steady_clock::now();
            if (state->should_reconnect(now, std::chrono::seconds(10))) {
                throw std::runtime_error("WHIP ICE connection failed or remained disconnected");
            }
            const bool track_open = track->isOpen();
            if (state->is_disconnected() || !track_open) {
                if (!media_opened && now >= connection_deadline) {
                    throw std::runtime_error("WHIP media track did not open");
                }
                if (media_opened && !track_open) {
                    if (!track_closed_at) track_closed_at = now;
                    if (now - *track_closed_at >= std::chrono::seconds(10)) {
                        throw std::runtime_error("WHIP media track remained closed");
                    }
                }
                if (!awaiting_resume) media_queue_.discard_all();
                awaiting_resume = true;
                std::unique_lock<std::mutex> lock(wait_mutex_);
                wait_changed_.wait_for(lock, std::chrono::milliseconds(100),
                                       [this] { return stopping_.load(); });
                continue;
            }
            if (awaiting_resume) {
                waiting_for_keyframe = true;
                timestamp_ready = false;
                awaiting_resume = false;
            }
            media_opened = true;
            track_closed_at.reset();
            auto unit = media_queue_.pop_for(std::chrono::milliseconds(100));
            if (!unit) continue;
            const auto drops = media_queue_.stats().dropped;
            if (unit->discontinuity || drops != observed_drops) {
                waiting_for_keyframe = true;
                timestamp_ready = false;
            }
            observed_drops = drops;
            if (waiting_for_keyframe && !unit->keyframe) continue;
            waiting_for_keyframe = false;
            if (unit->has_pts) {
                rtp->timestamp = h264_rtp_timestamp(unit->pts_ns);
                timestamp_ready = true;
            } else if (!timestamp_ready) {
                rtp->timestamp = rtp->startTimestamp;
                timestamp_ready = true;
            } else {
                rtp->timestamp += 3000;
            }
            if (rtp->timestampToSeconds(rtp->timestamp -
                                        reporter->lastReportedTimestamp()) > 1.0) {
                reporter->setNeedsToReport();
            }
            if (!track->send(reinterpret_cast<const std::byte*>(unit->bytes.data()),
                             unit->bytes.size())) {
                throw std::runtime_error("WHIP media track rejected an access unit");
            }
            if (++sent_units == 1) {
                logger_.log(LogLevel::Info, "whip", "first H.264 access unit sent");
            }
        }
    } catch (...) {
        peer->resetCallbacks();
        peer->close();
        close_resource();
        throw;
    }
    peer->resetCallbacks();
    peer->close();
    close_resource();
}

} // namespace skai
