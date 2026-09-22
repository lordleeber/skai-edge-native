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
BrowserOffer make_browser_offer(
        rtc::Description::Direction direction = rtc::Description::Direction::RecvOnly) {
    BrowserOffer result;
    result.peer = std::make_shared<rtc::PeerConnection>();
    rtc::Description::Video media("video", direction);
    media.addH264Codec(96);
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

TEST(WebRtcManager, DeliversAnnexBAccessUnitsOverTheNegotiatedH264Track) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto config = loopback_config(logger);
    skai::WebRtcManager manager(logger);
    manager.configure(config.webrtc);

    auto browser = make_browser_offer();
    browser.track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::StartSequence));
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
    EXPECT_TRUE(manager.close_session(created.session_id));
    EXPECT_EQ(manager.session_count(), 0U);
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
    EXPECT_EQ(request(server.port(), {http::verb::get, "/health", 11}).result(),
              http::status::ok);
    abandoned.set_option(asio::socket_base::linger(true, 0));
    abandoned.close();
    while (!manager->completed) std::this_thread::yield();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (manager->session_count() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(manager->session_count(), 0U);
}
