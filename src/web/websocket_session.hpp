#pragma once

#include "skai/api_state.hpp"
#include "skai/events.hpp"
#include "skai/status.hpp"
#include "skai/webrtc/webrtc_manager.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace skai::web {

class WebSocketSession final : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(boost::beast::tcp_stream stream,
                     std::chrono::steady_clock::time_point started,
                     std::shared_ptr<RuntimeStatus> status,
                     std::shared_ptr<ApiState> api,
                     std::shared_ptr<EventChannel> events,
                     std::shared_ptr<WebRtcManager> webrtc,
                     std::function<void(std::shared_ptr<WebSocketSession>)>
                         register_session,
                     std::function<void(WebSocketSession*)> unregister_session);
    ~WebSocketSession();

    void run(boost::beast::http::request<boost::beast::http::string_body> request);
    void stop();

private:
    void on_accept(boost::beast::error_code error);
    void enqueue(std::string event);
    void drain_incoming();
    void write_next();
    void on_write(boost::beast::error_code error);
    void read_next();
    void on_read(boost::beast::error_code error);
    void schedule_status();
    void begin_close();
    void cleanup();
    std::string status_data() const;
    std::string gps_data() const;

    boost::beast::websocket::stream<boost::beast::tcp_stream> stream_;
    boost::asio::steady_timer status_timer_;
    boost::beast::flat_buffer read_buffer_;
    std::chrono::steady_clock::time_point started_;
    std::shared_ptr<RuntimeStatus> status_;
    std::shared_ptr<ApiState> api_;
    std::shared_ptr<EventChannel> events_;
    std::shared_ptr<WebRtcManager> webrtc_;
    std::function<void(std::shared_ptr<WebSocketSession>)> register_session_;
    std::function<void(WebSocketSession*)> unregister_session_;
    EventQueue outgoing_{32};
    EventInbox incoming_{32};
    std::string writing_;
    std::uint64_t subscription_id_ = 0;
    bool closed_ = false;
    bool stopping_ = false;
    bool closing_ = false;
};

} // namespace skai::web
