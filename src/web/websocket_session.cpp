#include "websocket_session.hpp"

#include <boost/asio/post.hpp>

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace skai::web {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;

constexpr auto status_interval = std::chrono::seconds(1);
constexpr std::uint64_t message_limit = 64 * 1024;

} // namespace

WebSocketSession::WebSocketSession(beast::tcp_stream stream,
                                   std::chrono::steady_clock::time_point started,
                                   std::shared_ptr<RuntimeStatus> status,
                                   std::shared_ptr<ApiState> api,
                                   std::shared_ptr<EventChannel> events,
                                   std::function<void(
                                       std::shared_ptr<WebSocketSession>)>
                                       register_session,
                                   std::function<void(WebSocketSession*)>
                                       unregister_session)
    : stream_(std::move(stream)), status_timer_(stream_.get_executor()),
      started_(started), status_(std::move(status)), api_(std::move(api)),
      events_(std::move(events)),
      register_session_(std::move(register_session)),
      unregister_session_(std::move(unregister_session)) {}

WebSocketSession::~WebSocketSession() {
    if (subscription_id_) events_->unsubscribe(subscription_id_);
}

void WebSocketSession::run(
    beast::http::request<beast::http::string_body> request) {
    beast::get_lowest_layer(stream_).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(
        beast::role_type::server);
    timeout.handshake_timeout = std::chrono::seconds(5);
    timeout.idle_timeout = std::chrono::seconds(30);
    timeout.keep_alive_pings = true;
    stream_.set_option(timeout);
    stream_.read_message_max(message_limit);
    stream_.text(true);
    stream_.async_accept(request,
                         [self = shared_from_this()](beast::error_code error) {
                             self->on_accept(error);
                         });
}

void WebSocketSession::stop() {
    if (closed_ || stopping_) return;
    stopping_ = true;
    status_timer_.cancel();
    if (subscription_id_) {
        events_->unsubscribe(subscription_id_);
        subscription_id_ = 0;
    }
    if (writing_.empty()) begin_close();
}

void WebSocketSession::on_accept(beast::error_code error) {
    if (error) return cleanup();
    register_session_(shared_from_this());
    const std::weak_ptr<WebSocketSession> weak = shared_from_this();
    subscription_id_ = events_->subscribe([weak](const std::string& event) {
        if (const auto self = weak.lock()) {
            asio::post(self->stream_.get_executor(), [self, event] {
                self->enqueue(event);
            });
        }
    });
    enqueue(make_event_json(EventType::Status, status_data()));
    enqueue(make_event_json(EventType::Gps, gps_data()));
    schedule_status();
    read_next();
}

void WebSocketSession::enqueue(std::string event) {
    if (closed_ || stopping_) return;
    const bool idle = writing_.empty();
    outgoing_.push(std::move(event));
    if (idle) write_next();
}

void WebSocketSession::write_next() {
    const auto next = outgoing_.pop();
    if (!next) return;
    writing_ = std::move(*next);
    stream_.async_write(asio::buffer(writing_),
                        [self = shared_from_this()](beast::error_code error,
                                                    std::size_t) {
                            self->on_write(error);
                        });
}

void WebSocketSession::on_write(beast::error_code error) {
    if (error) return cleanup();
    writing_.clear();
    if (stopping_) return begin_close();
    write_next();
}

void WebSocketSession::read_next() {
    stream_.async_read(read_buffer_,
                       [self = shared_from_this()](beast::error_code error,
                                                   std::size_t) {
                           self->on_read(error);
                       });
}

void WebSocketSession::on_read(beast::error_code error) {
    if (error) return cleanup();
    read_buffer_.consume(read_buffer_.size());
    read_next();
}

void WebSocketSession::schedule_status() {
    status_timer_.expires_after(status_interval);
    status_timer_.async_wait([self = shared_from_this()](beast::error_code error) {
        if (error || self->closed_) return;
        self->enqueue(make_event_json(EventType::Status, self->status_data()));
        self->schedule_status();
    });
}

void WebSocketSession::begin_close() {
    if (closed_ || closing_) return;
    closing_ = true;
    stream_.async_close(websocket::close_code::going_away,
                        [self = shared_from_this()](beast::error_code) {
                            self->cleanup();
                        });
}

void WebSocketSession::cleanup() {
    if (closed_) return;
    closed_ = true;
    unregister_session_(this);
    status_timer_.cancel();
    if (subscription_id_) {
        events_->unsubscribe(subscription_id_);
        subscription_id_ = 0;
    }
    beast::error_code ignored;
    stream_.next_layer().socket().shutdown(
        boost::asio::ip::tcp::socket::shutdown_both, ignored);
    stream_.next_layer().socket().close(ignored);
}

std::string WebSocketSession::status_data() const {
    auto snapshot = status_->snapshot();
    snapshot.uptime_s = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_).count());
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(6) << "{\"status\":\"" << snapshot.status
           << "\",\"uptime_s\":" << snapshot.uptime_s
           << ",\"video_fps\":";
    if (snapshot.video_fps) output << *snapshot.video_fps;
    else output << "null";
    output << ",\"detector_enabled\":"
           << (api_->detector_enabled() ? "true" : "false")
           << ",\"detector_fps\":";
    if (snapshot.detector_fps) output << *snapshot.detector_fps;
    else output << "null";
    output << ",\"last_inference_ms\":";
    if (snapshot.last_inference_ms) output << *snapshot.last_inference_ms;
    else output << "null";
    output << '}';
    return output.str();
}

std::string WebSocketSession::gps_data() const {
    PublicConfigDto config;
    if (!api_->public_config(config) || !config.gps_enabled) {
        return "{\"available\":false}";
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"available\":true,\"source\":\"fixed\",\"latitude\":"
           << config.gps_latitude << ",\"longitude\":" << config.gps_longitude
           << ",\"altitude_m\":" << config.gps_altitude_m << '}';
    return output.str();
}

} // namespace skai::web
