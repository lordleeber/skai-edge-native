#include "skai/web/http_server.hpp"

#include "skai/web/router.hpp"
#include "static_file_handler.hpp"
#include "websocket_session.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace skai::web {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::uint32_t header_limit = 16 * 1024;
constexpr std::uint64_t body_limit = 64 * 1024;
constexpr auto request_timeout = std::chrono::seconds(5);

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, std::chrono::steady_clock::time_point started,
                std::shared_ptr<RuntimeStatus> status, std::shared_ptr<ApiState> api,
                std::shared_ptr<EventChannel> events,
                std::shared_ptr<AlertRepository> alerts,
                std::shared_ptr<RecordingController> recording,
                std::shared_ptr<WebRtcManager> webrtc,
                std::function<MetricsSnapshot()> metrics_provider,
                asio::thread_pool& signaling_pool,
                std::atomic_size_t& signaling_jobs,
                std::size_t signaling_limit,
                std::shared_ptr<StaticFileHandler> static_files,
                std::function<void(std::shared_ptr<WebSocketSession>)> register_ws,
                std::function<void(WebSocketSession*)> unregister_ws)
        : stream_(std::move(socket)), started_(started), status_(std::move(status)),
          api_(std::move(api)), events_(std::move(events)),
          alerts_(std::move(alerts)),
          recording_(std::move(recording)),
          webrtc_(std::move(webrtc)),
          metrics_provider_(std::move(metrics_provider)),
          signaling_pool_(signaling_pool), signaling_jobs_(signaling_jobs),
          signaling_limit_(signaling_limit),
          static_files_(std::move(static_files)),
          register_ws_(std::move(register_ws)),
          unregister_ws_(std::move(unregister_ws)) {
        parser_.header_limit(header_limit);
        parser_.body_limit(body_limit);
    }

    void run() {
        stream_.expires_after(request_timeout);
        http::async_read(stream_, buffer_, parser_,
                         [self = shared_from_this()](beast::error_code error,
                                                     std::size_t) {
                             self->on_read(error);
                         });
    }

private:
    void on_read(beast::error_code error) {
        if (error == http::error::body_limit) {
            send(json_error(http::status::payload_too_large, "payload too large"));
            return;
        }
        if (error == http::error::header_limit) {
            send(json_error(http::status::request_header_fields_too_large,
                            "headers too large"));
            return;
        }
        if (error) return close();
        if (beast::websocket::is_upgrade(parser_.get()) &&
            parser_.get().target() == "/ws") {
            auto request = parser_.release();
            auto session = std::make_shared<WebSocketSession>(
                std::move(stream_), started_, status_, api_, events_, webrtc_, register_ws_,
                unregister_ws_);
            session->run(std::move(request));
            return;
        }
        const auto target = parser_.get().target();
        const auto query = target.find('?');
        const auto path = target.substr(0, query);
        if (path != "/health" && path.find("/api/") != 0 && path != "/ws") {
            send(static_files_->handle(parser_.get()));
            return;
        }
        auto status = status_->snapshot();
        status.uptime_s = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started_).count());
        if (path == "/api/v1/webrtc/whep" &&
            parser_.get().method() == http::verb::post && webrtc_) {
            return dispatch_whep(parser_.release(), status);
        }
        if (path == "/api/v1/metrics") {
            const auto metrics = metrics_provider_();
            return send(route_request(parser_.get(), status, *api_, alerts_.get(),
                                      recording_.get(), webrtc_.get(), &metrics));
        }
        send(route_request(parser_.get(), status, *api_, alerts_.get(),
                           recording_.get(), webrtc_.get()));
    }

    void dispatch_whep(Request request, StatusSnapshot status) {
        if (signaling_jobs_.fetch_add(1) >= signaling_limit_) {
            signaling_jobs_.fetch_sub(1);
            return send(json_error(http::status::too_many_requests,
                                   "signaling queue is full"));
        }
        auto self = shared_from_this();
        asio::post(signaling_pool_, [self, request = std::move(request), status]() mutable {
            auto response = route_request(request, status, *self->api_, self->alerts_.get(),
                                          self->recording_.get(), self->webrtc_.get());
            self->signaling_jobs_.fetch_sub(1);
            asio::post(self->stream_.get_executor(),
                       [self, response = std::move(response)]() mutable {
                           self->send(std::move(response));
                       });
        });
    }

    Response json_error(http::status result, const char* message) {
        Response response{result, 11};
        response.set(http::field::content_type, "application/json");
        response.body() = std::string("{\"error\":\"") + message + "\"}\n";
        response.prepare_payload();
        return response;
    }

    void send(Response response) {
        constexpr std::string_view prefix = "/api/v1/webrtc/sessions/";
        const std::string location(response[http::field::location]);
        if (response.result() == http::status::created &&
            location.rfind(prefix, 0) == 0) {
            pending_session_id_ = location.substr(prefix.size());
        }
        response.keep_alive(false);
        response_ = std::move(response);
        stream_.expires_after(request_timeout);
        http::async_write(stream_, response_,
                          [self = shared_from_this()](beast::error_code error,
                                                      std::size_t) {
                              if (error && !self->pending_session_id_.empty() && self->webrtc_) {
                                  self->webrtc_->close_session(self->pending_session_id_);
                              }
                              self->pending_session_id_.clear();
                              self->close();
                          });
    }

    void close() {
        beast::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
        stream_.socket().close(ignored);
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    std::chrono::steady_clock::time_point started_;
    std::shared_ptr<RuntimeStatus> status_;
    std::shared_ptr<ApiState> api_;
    std::shared_ptr<EventChannel> events_;
    std::shared_ptr<AlertRepository> alerts_;
    std::shared_ptr<RecordingController> recording_;
    std::shared_ptr<WebRtcManager> webrtc_;
    std::function<MetricsSnapshot()> metrics_provider_;
    asio::thread_pool& signaling_pool_;
    std::atomic_size_t& signaling_jobs_;
    const std::size_t signaling_limit_;
    std::shared_ptr<StaticFileHandler> static_files_;
    std::function<void(std::shared_ptr<WebSocketSession>)> register_ws_;
    std::function<void(WebSocketSession*)> unregister_ws_;
    Response response_;
    std::string pending_session_id_;
};

} // namespace

struct HttpServer::State {
    State(Logger& logger, std::shared_ptr<RuntimeStatus> status,
          std::shared_ptr<ApiState> api, std::shared_ptr<EventChannel> events,
          std::shared_ptr<AlertRepository> alerts,
          std::shared_ptr<RecordingController> recording,
          std::shared_ptr<WebRtcManager> webrtc,
          std::size_t config_max_peers,
          const std::string& web_root,
          const std::string& recording_directory,
          std::function<MetricsSnapshot()> metrics_provider)
        : acceptor(context), shutdown_timer(context), logger(logger),
          status(std::move(status)), api(std::move(api)),
          events(std::move(events)),
          alerts(std::move(alerts)),
          recording(std::move(recording)),
          webrtc(std::move(webrtc)),
          signaling_limit(std::max<std::size_t>(1, config_max_peers)),
          static_files(std::make_shared<StaticFileHandler>(web_root)),
          system_metrics(recording_directory),
          metrics_provider(std::move(metrics_provider)) {}

    MetricsSnapshot snapshot_metrics() {
        auto metrics = metrics_provider ? metrics_provider() : MetricsSnapshot{};
        sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
            [](const auto& weak) { return weak.expired(); }), sessions.end());
        metrics.websocket_clients = sessions.size();
        std::string error;
        if (alerts) metrics.alert_count = alerts->count(error);
        static_cast<SystemMetrics&>(metrics) = system_metrics.sample();
        return metrics;
    }

    void accept() {
        acceptor.async_accept([this](beast::error_code error, tcp::socket socket) {
            if (!error) {
                std::make_shared<HttpSession>(std::move(socket), started, status,
                                              api, events,
                                              alerts,
                                              recording,
                                              webrtc,
                                              [this] { return snapshot_metrics(); },
                                              signaling_pool,
                                              signaling_jobs,
                                              signaling_limit,
                                              static_files,
                                              [this](auto session) {
                                                  sessions.erase(std::remove_if(
                                                      sessions.begin(), sessions.end(),
                                                      [](const auto& weak) {
                                                          return weak.expired();
                                                  }), sessions.end());
                                                  sessions.push_back(session);
                                              },
                                              [this](WebSocketSession* session) {
                                                  sessions.erase(std::remove_if(
                                                      sessions.begin(), sessions.end(),
                                                      [session](const auto& weak) {
                                                          const auto current = weak.lock();
                                                          return !current ||
                                                                 current.get() == session;
                                                      }), sessions.end());
                                              })->run();
            } else if (error != asio::error::operation_aborted) {
                logger.log(LogLevel::Error, "web", error.message());
            }
            if (acceptor.is_open()) accept();
        });
    }

    asio::io_context context{1};
    tcp::acceptor acceptor;
    asio::steady_timer shutdown_timer;
    Logger& logger;
    std::shared_ptr<RuntimeStatus> status;
    std::shared_ptr<ApiState> api;
    std::shared_ptr<EventChannel> events;
    std::shared_ptr<AlertRepository> alerts;
    std::shared_ptr<RecordingController> recording;
    std::shared_ptr<WebRtcManager> webrtc;
    std::atomic_size_t signaling_jobs{0};
    std::size_t signaling_limit;
    asio::thread_pool signaling_pool{2};
    std::shared_ptr<StaticFileHandler> static_files;
    SystemMetricsSampler system_metrics;
    std::function<MetricsSnapshot()> metrics_provider;
    std::vector<std::weak_ptr<WebSocketSession>> sessions;
    std::thread worker;
    std::chrono::steady_clock::time_point started;
    unsigned short port = 0;
};

HttpServer::HttpServer(Logger& logger)
    : HttpServer(logger, std::make_shared<RuntimeStatus>(),
                 std::make_shared<ApiState>(), std::make_shared<EventChannel>()) {}

HttpServer::HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status)
    : HttpServer(logger, std::move(status), std::make_shared<ApiState>(),
                 std::make_shared<EventChannel>()) {}

HttpServer::HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status,
                       std::shared_ptr<ApiState> api)
    : HttpServer(logger, std::move(status), std::move(api),
                 std::make_shared<EventChannel>()) {}

HttpServer::HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status,
                       std::shared_ptr<ApiState> api,
                       std::shared_ptr<EventChannel> events)
    : HttpServer(logger, std::move(status), std::move(api), std::move(events),
                 nullptr) {}

HttpServer::HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status,
                       std::shared_ptr<ApiState> api,
                       std::shared_ptr<EventChannel> events,
                       std::shared_ptr<AlertRepository> alerts,
                       std::shared_ptr<RecordingController> recording,
                       std::shared_ptr<WebRtcManager> webrtc,
                       std::function<MetricsSnapshot()> metrics_provider)
    : logger_(logger), status_(status ? std::move(status)
                                     : std::make_shared<RuntimeStatus>()),
      api_(api ? std::move(api) : std::make_shared<ApiState>()),
      events_(events ? std::move(events) : std::make_shared<EventChannel>()),
      alerts_(std::move(alerts)), recording_(std::move(recording)),
      webrtc_(std::move(webrtc)), metrics_provider_(std::move(metrics_provider)) {
    api_->bind_runtime_status(status_);
}

HttpServer::~HttpServer() {
    stop();
    wait();
}

bool HttpServer::initialize(const Config& config) {
    if (state_) return false;
    api_->configure(config);
    auto next = std::make_unique<State>(logger_, status_, api_, events_, alerts_, recording_,
                                        webrtc_, config.webrtc.max_peers, config.web.root,
                                        config.recording.directory, metrics_provider_);
    if (!next->static_files->valid()) {
        logger_.log(LogLevel::Error, "web", next->static_files->error());
        return false;
    }
    beast::error_code error;
    const auto address = asio::ip::make_address(config.web.bind, error);
    if (error) {
        logger_.log(LogLevel::Error, "web", "web.bind must be an IP address");
        return false;
    }
    const tcp::endpoint endpoint(address,
                                 static_cast<unsigned short>(config.web.port));
    next->acceptor.open(endpoint.protocol(), error);
    if (!error) next->acceptor.set_option(asio::socket_base::reuse_address(true), error);
    if (!error) next->acceptor.bind(endpoint, error);
    if (!error) next->acceptor.listen(asio::socket_base::max_listen_connections, error);
    if (!error) next->port = next->acceptor.local_endpoint(error).port();
    if (error) {
        logger_.log(LogLevel::Error, "web", "cannot listen: " + error.message());
        return false;
    }
    if (webrtc_) webrtc_->configure(config.webrtc);
    state_ = std::move(next);
    return true;
}

bool HttpServer::start() {
    if (!state_ || state_->worker.joinable()) return false;
    state_->started = std::chrono::steady_clock::now();
    state_->accept();
    state_->worker = std::thread([this] { state_->context.run(); });
    logger_.log(LogLevel::Info, "web", "HTTP server started");
    return true;
}

void HttpServer::stop() noexcept {
    if (webrtc_) webrtc_->shutdown();
    if (!state_) return;
    if (!state_->worker.joinable()) {
        beast::error_code ignored;
        state_->acceptor.cancel(ignored);
        state_->acceptor.close(ignored);
        state_->context.stop();
        return;
    }
    auto* state = state_.get();
    try {
        asio::post(state->context, [state] {
            beast::error_code ignored;
            state->acceptor.cancel(ignored);
            state->acceptor.close(ignored);
            state->sessions.erase(std::remove_if(
                state->sessions.begin(), state->sessions.end(),
                [](const auto& weak) { return weak.expired(); }),
                state->sessions.end());
            bool has_websockets = false;
            for (const auto& weak : state->sessions) {
                if (const auto session = weak.lock()) {
                    has_websockets = true;
                    session->stop();
                }
            }
            if (!has_websockets) return state->context.stop();
            state->shutdown_timer.expires_after(std::chrono::milliseconds(250));
            state->shutdown_timer.async_wait([state](beast::error_code) {
                state->context.stop();
            });
        });
    } catch (...) {
        state->context.stop();
    }
}

void HttpServer::wait() noexcept {
    if (!state_) return;
    if (state_->worker.joinable()) state_->worker.join();
    state_.reset();
}

unsigned short HttpServer::port() const noexcept {
    return state_ ? state_->port : 0;
}

} // namespace skai::web
