#include "skai/web/http_server.hpp"

#include "skai/web/router.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

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
                std::shared_ptr<RuntimeStatus> status, std::shared_ptr<ApiState> api)
        : stream_(std::move(socket)), started_(started), status_(std::move(status)),
          api_(std::move(api)) {
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
        auto status = status_->snapshot();
        status.uptime_s = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started_).count());
        send(route_request(parser_.get(), status, *api_));
    }

    Response json_error(http::status result, const char* message) {
        Response response{result, 11};
        response.set(http::field::content_type, "application/json");
        response.body() = std::string("{\"error\":\"") + message + "\"}\n";
        response.prepare_payload();
        return response;
    }

    void send(Response response) {
        response.keep_alive(false);
        response_ = std::move(response);
        stream_.expires_after(request_timeout);
        http::async_write(stream_, response_,
                          [self = shared_from_this()](beast::error_code,
                                                      std::size_t) {
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
    Response response_;
};

} // namespace

struct HttpServer::State {
    State(Logger& logger, std::shared_ptr<RuntimeStatus> status,
          std::shared_ptr<ApiState> api)
        : acceptor(context), logger(logger), status(std::move(status)),
          api(std::move(api)) {}

    void accept() {
        acceptor.async_accept([this](beast::error_code error, tcp::socket socket) {
            if (!error) {
                std::make_shared<HttpSession>(std::move(socket), started, status,
                                              api)->run();
            } else if (error != asio::error::operation_aborted) {
                logger.log(LogLevel::Error, "web", error.message());
            }
            if (acceptor.is_open()) accept();
        });
    }

    asio::io_context context{1};
    tcp::acceptor acceptor;
    Logger& logger;
    std::shared_ptr<RuntimeStatus> status;
    std::shared_ptr<ApiState> api;
    std::thread worker;
    std::chrono::steady_clock::time_point started;
    unsigned short port = 0;
};

HttpServer::HttpServer(Logger& logger)
    : HttpServer(logger, std::make_shared<RuntimeStatus>(),
                 std::make_shared<ApiState>()) {}

HttpServer::HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status)
    : HttpServer(logger, std::move(status), std::make_shared<ApiState>()) {}

HttpServer::HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status,
                       std::shared_ptr<ApiState> api)
    : logger_(logger), status_(status ? std::move(status)
                                     : std::make_shared<RuntimeStatus>()),
      api_(api ? std::move(api) : std::make_shared<ApiState>()) {}

HttpServer::~HttpServer() {
    stop();
    wait();
}

bool HttpServer::initialize(const Config& config) {
    if (state_) return false;
    api_->configure(config);
    auto next = std::make_unique<State>(logger_, status_, api_);
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
            state->context.stop();
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
