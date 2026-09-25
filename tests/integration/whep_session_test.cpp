#include "skai/webrtc/ice_runtime_module.hpp"
#include "skai/webrtc/webrtc_manager.hpp"
#include "skai/web/http_server.hpp"

#include <rtc/rtc.hpp>

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct BrowserOffer {
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::Track> track;
    std::string sdp;
};
class NackFirstPacket final : public rtc::MediaHandler {
public:
    void incoming(rtc::message_vector& messages,
                  const rtc::message_callback& send) override {
        if (sent_) return;
        for (const auto& message : messages) {
            if (message->type == rtc::Message::Control ||
                message->size() < sizeof(rtc::RtpHeader)) continue;
            auto* rtp = reinterpret_cast<rtc::RtpHeader*>(message->data());
            auto nack_message = rtc::make_message(rtc::RtcpNack::Size(1),
                                                   rtc::Message::Control);
            auto* nack = reinterpret_cast<rtc::RtcpNack*>(nack_message->data());
            nack->preparePacket(rtp->ssrc(), 1);
            nack->parts[0].setPid(rtp->seqNumber());
            nack->parts[0].setBlp(0);
            sent_ = true;
            send(std::move(nack_message));
            break;
        }
    }

private:
    bool sent_ = false;
};
class SlowWebRtcManager final : public skai::WebRtcManager {
public:
    using WebRtcManager::WebRtcManager;
    skai::CreateSessionResult create_session(std::string_view offer) override {
        entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto result = WebRtcManager::create_session(offer);
        completed = true;
        return result;
    }
    std::atomic_bool entered{false}, completed{false};
};
class FailingPeerWebRtcManager final : public skai::WebRtcManager {
public:
    using WebRtcManager::WebRtcManager;

protected:
    std::shared_ptr<skai::WebRtcSession> create_peer_session(
        std::string, std::size_t, std::atomic<std::uint64_t>*) override {
        throw std::runtime_error("PeerConnection construction failed");
    }
};
BrowserOffer make_browser_offer(
        rtc::Description::Direction direction = rtc::Description::Direction::RecvOnly,
        std::string profile = rtc::DEFAULT_H264_VIDEO_PROFILE) {
    BrowserOffer result;
    result.peer = std::make_shared<rtc::PeerConnection>();
    rtc::Description::Video media("video", direction);
    media.addH264Codec(96, std::move(profile));
    result.track = result.peer->addTrack(media);

    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false;
    result.peer->onGatheringStateChange(
        [&](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                complete = true;
            }
            changed.notify_all();
        });
    result.peer->setLocalDescription(rtc::Description::Type::Offer);
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return complete; })) {
        return {};
    }
    lock.unlock();
    const auto local = result.peer->localDescription();
    if (local) result.sdp = std::string(*local);
    result.peer->resetCallbacks();
    return result;
}

http::response<http::string_body> request(
        unsigned short port, http::request<http::string_body> outgoing) {
    asio::io_context context;
    beast::tcp_stream stream(context);
    stream.connect({asio::ip::make_address("127.0.0.1"), port});
    outgoing.set(http::field::host, "127.0.0.1");
    outgoing.prepare_payload();
    http::write(stream, outgoing);
    beast::flat_buffer buffer;
    http::response<http::string_body> incoming;
    http::read(stream, buffer, incoming);
    return incoming;
}

skai::Config loopback_config(skai::Logger& logger) {
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    config.webrtc.host_interfaces = {"lo"};
    skai::IceRuntimeModule runtime(logger);
    EXPECT_TRUE(runtime.initialize(config)) << runtime.last_error();
    return config;
}

skai::EncodedAccessUnit h264_keyframe(std::uint64_t pts_ns) {
    skai::EncodedAccessUnit unit;
    unit.pts_ns = pts_ns;
    unit.keyframe = true;
    unit.bytes = {
        0, 0, 0, 1, 0x67, 0x42, 0xe0, 0x1f, 0x95, 0xa8, 0x14, 0x01,
        0x6e, 0x9b, 0x80,
        0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
        0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00};
    return unit;
}

skai::EncodedAccessUnit h264_delta(std::uint64_t pts_ns) {
    skai::EncodedAccessUnit unit;
    unit.pts_ns = pts_ns;
    unit.bytes = {0, 0, 0, 1, 0x41, 0x9a, 0x20, 0x00};
    return unit;
}

TEST(WebRtcManager, InvalidatesCachedKeyframeOnSourceDiscontinuity) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::WebRtcManager manager(logger);
    auto config = loopback_config(logger);
    manager.configure(config.webrtc);

    manager.publish_access_unit(h264_keyframe(0));
    EXPECT_TRUE(manager.diagnostics().keyframe_cached);
    auto discontinuity = h264_delta(33'333'333ULL);
    discontinuity.discontinuity = true;
    manager.publish_access_unit(discontinuity);
    EXPECT_FALSE(manager.diagnostics().keyframe_cached);
    manager.publish_access_unit(h264_keyframe(66'666'666ULL));
    EXPECT_TRUE(manager.diagnostics().keyframe_cached);
}

TEST(WebRtcManager, RejectsSessionWhileSourceMediaIsUnsupported) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::WebRtcManager manager(logger);
    auto config = loopback_config(logger);
    manager.configure(config.webrtc);
    manager.set_media_available(false, "recording and WebRTC require H.264 input");

    const auto created = manager.create_session(make_browser_offer().sdp);
    EXPECT_EQ(created.error, skai::CreateSessionError::Disabled);
    EXPECT_NE(created.message.find("require H.264"), std::string::npos);
}

bool contains_nal_type(const rtc::binary& bytes, std::uint8_t type) {
    for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
        const auto value = [&](std::size_t position) {
            return std::to_integer<std::uint8_t>(bytes[position]);
        };
        const bool long_start = value(index) == 0 && value(index + 1) == 0 &&
                                value(index + 2) == 0 && value(index + 3) == 1;
        const bool short_start = value(index) == 0 && value(index + 1) == 0 &&
                                 value(index + 2) == 1;
        const auto header = index + (long_start ? 4 : 3);
        if ((long_start || short_start) && header < bytes.size() &&
            (value(header) & 0x1f) == type) return true;
    }
    return false;
}

} // namespace

TEST(WebRtcManager, CreatesUniqueAnswersEnforcesCapacityAndClosesSessions) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    config.webrtc.max_peers = 1;
    skai::WebRtcManager manager(logger);
    manager.configure(config.webrtc);

    const auto malformed = manager.create_session("not an SDP offer");
    EXPECT_EQ(malformed.error, skai::CreateSessionError::InvalidOffer);
    EXPECT_EQ(manager.session_count(), 0U);
    const auto failed_signaling = manager.diagnostics();
    ASSERT_EQ(failed_signaling.recently_closed.size(), 1U);
    EXPECT_EQ(failed_signaling.recently_closed.back().failure_stage, "signaling");
    EXPECT_EQ(failed_signaling.recently_closed.back().close_reason,
              "signaling_failure");

    const auto excessive_level = manager.create_session(make_browser_offer(
        rtc::Description::Direction::RecvOnly,
        "profile-level-id=42e029;packetization-mode=1;level-asymmetry-allowed=1").sdp);
    EXPECT_EQ(excessive_level.error, skai::CreateSessionError::InvalidOffer);
    EXPECT_NE(excessive_level.message.find("H.264"), std::string::npos);

    for (const auto direction : {rtc::Description::Direction::SendOnly,
                                 rtc::Description::Direction::Inactive}) {
        const auto forbidden = manager.create_session(make_browser_offer(direction).sdp);
        EXPECT_EQ(forbidden.error, skai::CreateSessionError::InvalidOffer);
        EXPECT_NE(forbidden.message.find("recvonly or sendrecv"), std::string::npos);
    }

    auto first_offer = make_browser_offer();
    const auto first = manager.create_session(first_offer.sdp);
    ASSERT_TRUE(first) << first.message;
    EXPECT_EQ(first.session_id.size(), 32U);
    EXPECT_NE(first.answer_sdp.find("a=ice-ufrag:"), std::string::npos);
    EXPECT_NE(first.answer_sdp.find("a=candidate:"), std::string::npos);
    EXPECT_NE(first.answer_sdp.find("a=sendonly"), std::string::npos);
    EXPECT_NE(first.answer_sdp.find("profile-level-id=42e01f"), std::string::npos);
    EXPECT_EQ(manager.session_count(), 1U);

    auto blocked_offer = make_browser_offer(rtc::Description::Direction::SendRecv);
    const auto blocked = manager.create_session(blocked_offer.sdp);
    EXPECT_EQ(blocked.error, skai::CreateSessionError::Capacity);
    EXPECT_TRUE(manager.close_session(first.session_id));
    EXPECT_FALSE(manager.close_session(first.session_id));

    const auto second = manager.create_session(blocked_offer.sdp);
    ASSERT_TRUE(second) << second.message;
    EXPECT_NE(second.session_id, first.session_id);
    EXPECT_NE(second.answer_sdp.find("a=sendonly"), std::string::npos);
    manager.shutdown();
    EXPECT_EQ(manager.session_count(), 0U);
    EXPECT_EQ(manager.create_session(blocked_offer.sdp).error,
              skai::CreateSessionError::Disabled);
}

TEST(WebRtcManager, PeerConstructionFailureDoesNotEscapeOrLeakSession) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    FailingPeerWebRtcManager manager(logger);
    manager.configure(config.webrtc);

    const auto result = manager.create_session(make_browser_offer().sdp);
    EXPECT_EQ(result.error, skai::CreateSessionError::Internal);
    EXPECT_EQ(manager.session_count(), 0U);
    const auto diagnostics = manager.diagnostics();
    EXPECT_EQ(diagnostics.signaling_errors, 1U);
    EXPECT_EQ(diagnostics.sessions_created, 0U);
}

TEST(WebRtcManager, DeliversAnnexBAccessUnitsOverTheNegotiatedH264Track) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    skai::WebRtcManager manager(logger);
    manager.configure(config.webrtc);

    auto browser = make_browser_offer();
    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::StartSequence);
    depacketizer->addToChain(std::make_shared<NackFirstPacket>());
    browser.track->setMediaHandler(depacketizer);
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t received_bytes = 0;
    browser.track->onFrame([&](rtc::binary data, rtc::FrameInfo) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            received_bytes += data.size();
        }
        changed.notify_all();
    });

    const auto created = manager.create_session(browser.sdp);
    ASSERT_TRUE(created) << created.message << '\n' << logs.str();
    browser.peer->setRemoteDescription(
        rtc::Description(created.answer_sdp, rtc::Description::Type::Answer));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint64_t pts_ns = 0;
    std::unique_lock<std::mutex> lock(mutex);
    while (received_bytes == 0 && std::chrono::steady_clock::now() < deadline) {
        lock.unlock();
        manager.publish_access_unit(h264_keyframe(pts_ns));
        pts_ns += 33'333'333;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        lock.lock();
    }
    EXPECT_GT(received_bytes, 0U) << logs.str();
    lock.unlock();
    auto diagnostics = manager.diagnostics();
    const auto nack_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!diagnostics.peers.empty() &&
           diagnostics.peers[0].packets_retransmitted == 0 &&
           std::chrono::steady_clock::now() < nack_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        diagnostics = manager.diagnostics();
    }
    ASSERT_EQ(diagnostics.peers.size(), 1U);
    EXPECT_EQ(diagnostics.peers[0].peer_state, "connected");
    EXPECT_TRUE(diagnostics.peers[0].ice_state == "connected" ||
                diagnostics.peers[0].ice_state == "completed");
    EXPECT_EQ(diagnostics.peers[0].local_interface, "lo");
    EXPECT_EQ(diagnostics.peers[0].selected_interface, "lo");
    EXPECT_FALSE(diagnostics.peers[0].local_candidate.empty());
    EXPECT_GT(diagnostics.peers[0].bytes_sent, 0U);
    EXPECT_GT(diagnostics.peers[0].packets_sent, 0U);
    EXPECT_GT(diagnostics.peers[0].packets_retransmitted, 0U);
    EXPECT_TRUE(manager.close_session(created.session_id));
    EXPECT_EQ(manager.session_count(), 0U);
    const auto closed = manager.diagnostics();
    EXPECT_EQ(closed.sessions_created, 1U);
    EXPECT_EQ(closed.sessions_closed, 1U);
    EXPECT_EQ(closed.last_close_reason, "client_delete");
    ASSERT_EQ(closed.recently_closed.size(), 1U);
    EXPECT_EQ(closed.recently_closed[0].session_id, created.session_id);
    EXPECT_EQ(closed.recently_closed[0].close_reason, "client_delete");
}

TEST(WebRtcManager, SupportsTwoConnectedViewersAndRepeatedSessionCleanup) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    config.webrtc.max_peers = 2;
    skai::WebRtcManager manager(logger);
    manager.configure(config.webrtc);
    auto first_browser = make_browser_offer();
    auto second_browser = make_browser_offer();
    std::atomic<int> first_frames{0}, second_frames{0};
    for (auto* browser : {&first_browser, &second_browser}) {
        browser->track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
            rtc::NalUnit::Separator::StartSequence));
    }
    first_browser.track->onFrame([&](rtc::binary, rtc::FrameInfo) { ++first_frames; });
    second_browser.track->onFrame([&](rtc::binary, rtc::FrameInfo) { ++second_frames; });

    const auto first = manager.create_session(first_browser.sdp);
    const auto second = manager.create_session(second_browser.sdp);
    ASSERT_TRUE(first) << first.message;
    ASSERT_TRUE(second) << second.message;
    first_browser.peer->setRemoteDescription(
        rtc::Description(first.answer_sdp, rtc::Description::Type::Answer));
    second_browser.peer->setRemoteDescription(
        rtc::Description(second.answer_sdp, rtc::Description::Type::Answer));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint64_t pts = 0;
    while ((first_frames.load() == 0 || second_frames.load() == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        manager.publish_access_unit(h264_keyframe(pts));
        pts += 33'333'333;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GT(first_frames.load(), 0);
    EXPECT_GT(second_frames.load(), 0);
    const auto active = manager.diagnostics();
    ASSERT_EQ(active.peers.size(), 2U);
    EXPECT_EQ(active.peers[0].peer_state, "connected");
    EXPECT_EQ(active.peers[1].peer_state, "connected");
    EXPECT_EQ(manager.create_session(make_browser_offer().sdp).error,
              skai::CreateSessionError::Capacity);
    EXPECT_TRUE(manager.close_session(first.session_id));
    EXPECT_TRUE(manager.close_session(second.session_id));

    for (int cycle = 0; cycle < 20; ++cycle) {
        const auto created = manager.create_session(make_browser_offer().sdp);
        ASSERT_TRUE(created) << "cycle " << cycle << ": " << created.message;
        ASSERT_TRUE(manager.close_session(created.session_id));
        EXPECT_EQ(manager.session_count(), 0U);
    }
    const auto diagnostics = manager.diagnostics();
    EXPECT_EQ(diagnostics.sessions_created, 22U);
    EXPECT_EQ(diagnostics.sessions_closed, 22U);
    EXPECT_EQ(diagnostics.recently_closed.size(), 16U);
}

TEST(WebRtcManager, KeepsRtpTimeMovingWhenEncoderPtsRestarts) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    skai::WebRtcManager manager(logger);
    manager.configure(config.webrtc);
    auto browser = make_browser_offer();
    browser.track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::StartSequence));
    std::mutex mutex;
    std::vector<std::uint32_t> timestamps;
    browser.track->onFrame([&](rtc::binary, rtc::FrameInfo frame) {
        std::lock_guard<std::mutex> lock(mutex);
        timestamps.push_back(frame.timestamp);
    });
    const auto created = manager.create_session(browser.sdp);
    ASSERT_TRUE(created) << created.message;
    browser.peer->setRemoteDescription(
        rtc::Description(created.answer_sdp, rtc::Description::Type::Answer));

    const auto open_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!browser.track->isOpen() && std::chrono::steady_clock::now() < open_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(browser.track->isOpen()) << logs.str();

    const auto media_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < media_deadline) {
        manager.publish_access_unit(h264_keyframe(600'000'000'000ULL));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lock(mutex);
        if (!timestamps.empty()) break;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        ASSERT_FALSE(timestamps.empty()) << logs.str();
        timestamps.clear();
    }

    const std::vector<skai::EncodedAccessUnit> units = {
        h264_keyframe(600'033'333'333ULL), h264_keyframe(0),
        h264_keyframe(33'333'333ULL)};
    for (const auto& unit : units) {
        std::size_t before;
        {
            std::lock_guard<std::mutex> lock(mutex);
            before = timestamps.size();
        }
        manager.publish_access_unit(unit);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard<std::mutex> lock(mutex);
            if (timestamps.size() > before) break;
        }
    }
    manager.close_session(created.session_id);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_GE(timestamps.size(), 3U) << logs.str();
    for (std::size_t index = timestamps.size() - 2; index < timestamps.size(); ++index) {
        const auto step = timestamps[index] - timestamps[index - 1];
        EXPECT_GT(step, 0U);
        EXPECT_LE(step, 90'000U);
    }
}

TEST(WebRtcManager, WaitsForKeyframeAfterPerPeerQueueDropsAccessUnits) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    skai::WebRtcManager manager(logger);
    manager.configure(config.webrtc);
    auto browser = make_browser_offer();
    browser.track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::StartSequence));
    std::mutex mutex;
    std::vector<bool> received_keyframes;
    browser.track->onFrame([&](rtc::binary data, rtc::FrameInfo) {
        std::lock_guard<std::mutex> lock(mutex);
        received_keyframes.push_back(contains_nal_type(data, 5));
    });
    const auto created = manager.create_session(browser.sdp);
    ASSERT_TRUE(created) << created.message;

    manager.publish_access_unit(h264_keyframe(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (std::uint64_t index = 1; index <= 20; ++index) {
        manager.publish_access_unit(h264_delta(index * 33'333'333ULL));
    }
    manager.publish_access_unit(h264_keyframe(21 * 33'333'333ULL));
    manager.publish_access_unit(h264_delta(22 * 33'333'333ULL));
    manager.publish_access_unit(h264_delta(23 * 33'333'333ULL));
    browser.peer->setRemoteDescription(
        rtc::Description(created.answer_sdp, rtc::Description::Type::Answer));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (received_keyframes.size() >= 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    manager.close_session(created.session_id);
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_GE(received_keyframes.size(), 2U) << logs.str();
    EXPECT_TRUE(received_keyframes[0]);
    EXPECT_TRUE(received_keyframes[1]);
}

TEST(WebRtcManager, ReapsStaleSessions) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    skai::WebRtcManager manager(logger, std::chrono::milliseconds(50));
    manager.configure(config.webrtc);
    auto offer = make_browser_offer();
    ASSERT_TRUE(manager.create_session(offer.sdp));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (manager.session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(manager.session_count(), 0U);
    const auto diagnostics = manager.diagnostics();
    EXPECT_EQ(diagnostics.sessions_closed, 1U);
    EXPECT_EQ(diagnostics.last_close_reason, "connection_timeout");
}

TEST(WebRtcManager, StartsConnectionTimeoutOnlyAfterSignalingCompletes) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    skai::WebRtcManager manager(logger, std::chrono::milliseconds(1));
    manager.configure(config.webrtc);
    auto offer = make_browser_offer().sdp;
    for (int index = 0; index < 3000; ++index) {
        offer += "a=x-skai-padding:" + std::to_string(index) + "\r\n";
    }

    const auto created = manager.create_session(offer);
    ASSERT_TRUE(created) << created.message << '\n' << logs.str();
}

TEST(WhepHttpApi, CreatesAndDeletesSessionWithoutWebSocket) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    auto manager = std::make_shared<skai::WebRtcManager>(logger);
    skai::web::HttpServer server(logger, std::make_shared<skai::RuntimeStatus>(),
        std::make_shared<skai::ApiState>(), std::make_shared<skai::EventChannel>(),
        nullptr, nullptr, manager);
    ASSERT_TRUE(server.initialize(config)) << logs.str();
    ASSERT_TRUE(server.start());

    http::request<http::string_body> bad{http::verb::post, "/api/v1/webrtc/whep", 11};
    bad.set(http::field::content_type, "application/sdp");
    bad.body() = "invalid";
    EXPECT_EQ(request(server.port(), std::move(bad)).result(), http::status::bad_request);
    http::request<http::string_body> wrong_type{
        http::verb::post, "/api/v1/webrtc/whep", 11};
    wrong_type.set(http::field::content_type, "application/json");
    EXPECT_EQ(request(server.port(), std::move(wrong_type)).result(),
              http::status::unsupported_media_type);
    EXPECT_EQ(request(server.port(),
        {http::verb::get, "/api/v1/webrtc/whep", 11})[http::field::allow], "POST");

    auto offer = make_browser_offer();
    http::request<http::string_body> post{http::verb::post, "/api/v1/webrtc/whep", 11};
    post.set(http::field::content_type, "application/sdp");
    post.body() = offer.sdp;
    const auto created = request(server.port(), std::move(post));
    ASSERT_EQ(created.result(), http::status::created) << created.body();
    EXPECT_EQ(created[http::field::content_type], "application/sdp");
    EXPECT_NE(created.body().find("a=ice-ufrag:"), std::string::npos);
    EXPECT_NO_THROW(offer.peer->setRemoteDescription(
        rtc::Description(created.body(), rtc::Description::Type::Answer)));
    const std::string location(created[http::field::location]);
    EXPECT_EQ(location.rfind("/api/v1/webrtc/sessions/", 0), 0U);
    EXPECT_EQ(manager->session_count(), 1U);
    http::request<http::string_body> remove{http::verb::delete_, location, 11};
    EXPECT_EQ(request(server.port(), std::move(remove)).result(), http::status::no_content);
    EXPECT_EQ(manager->session_count(), 0U);
    http::request<http::string_body> missing{http::verb::delete_, location, 11};
    EXPECT_EQ(request(server.port(), std::move(missing)).result(), http::status::not_found);

    auto shutdown_offer = make_browser_offer();
    http::request<http::string_body> active{http::verb::post, "/api/v1/webrtc/whep", 11};
    active.set(http::field::content_type, "application/sdp");
    active.body() = shutdown_offer.sdp;
    ASSERT_EQ(request(server.port(), std::move(active)).result(), http::status::created);
    ASSERT_EQ(manager->session_count(), 1U);
    server.stop();
    server.wait();
    EXPECT_EQ(manager->session_count(), 0U);
}

TEST(WhepHttpApi, KeepsControlPlaneResponsiveAndRollsBackAbandonedResponse) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    auto manager = std::make_shared<SlowWebRtcManager>(logger);
    skai::web::HttpServer server(logger, nullptr, nullptr, nullptr, nullptr, nullptr, manager);
    ASSERT_TRUE(server.initialize(config));
    ASSERT_TRUE(server.start());
    asio::io_context context;
    tcp::socket abandoned(context);
    abandoned.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    http::request<http::string_body> post{http::verb::post, "/api/v1/webrtc/whep", 11};
    post.set(http::field::content_type, "application/sdp");
    post.body() = make_browser_offer().sdp;
    post.prepare_payload();
    http::write(abandoned, post);
    while (!manager->entered) std::this_thread::yield();
    const auto health = request(server.port(), {http::verb::get, "/health", 11});
    EXPECT_EQ(health.result(), http::status::service_unavailable);
    EXPECT_NE(health.body().find("health watchdog unavailable"), std::string::npos);
    abandoned.set_option(asio::socket_base::linger(true, 0));
    abandoned.close();
    while (!manager->completed) std::this_thread::yield();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (manager->session_count() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(manager->session_count(), 0U);
}
