#include "skai/webrtc/ice_runtime_module.hpp"
#include "skai/webrtc/webrtc_manager.hpp"
#include "skai/web/http_server.hpp"

#include <rtc/rtc.hpp>

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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

BrowserOffer make_browser_offer() {
    BrowserOffer result;
    result.peer = std::make_shared<rtc::PeerConnection>();
    rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
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

    auto first_offer = make_browser_offer();
    ASSERT_FALSE(first_offer.sdp.empty());
    const auto first = manager.create_session(first_offer.sdp);
    ASSERT_TRUE(first) << first.message;
    EXPECT_EQ(first.session_id.size(), 32U);
    EXPECT_NE(first.answer_sdp.find("a=ice-ufrag:"), std::string::npos);
    EXPECT_NE(first.answer_sdp.find("a=candidate:"), std::string::npos);
    EXPECT_NE(first.answer_sdp.find("a=end-of-candidates"), std::string::npos);
    EXPECT_EQ(manager.session_count(), 1U);

    auto blocked_offer = make_browser_offer();
    const auto blocked = manager.create_session(blocked_offer.sdp);
    EXPECT_EQ(blocked.error, skai::CreateSessionError::Capacity);
    EXPECT_TRUE(manager.close_session(first.session_id));
    EXPECT_FALSE(manager.close_session(first.session_id));

    const auto second = manager.create_session(blocked_offer.sdp);
    ASSERT_TRUE(second) << second.message;
    EXPECT_NE(second.session_id, first.session_id);
    manager.shutdown();
    EXPECT_EQ(manager.session_count(), 0U);
    EXPECT_EQ(manager.create_session(blocked_offer.sdp).error,
              skai::CreateSessionError::Disabled);
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
