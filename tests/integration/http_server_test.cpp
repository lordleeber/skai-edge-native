#include "skai/web/http_server.hpp"
#include "skai/events.hpp"
#include "skai/status.hpp"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <sstream>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

http::response<http::string_body> request(unsigned short port,
                                          http::request<http::string_body> value) {
    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    http::write(socket, value);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
}

void connect_websocket(websocket::stream<tcp::socket>& client,
                       unsigned short port) {
    client.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
    client.handshake("127.0.0.1", "/ws");
}

std::string read_websocket(websocket::stream<tcp::socket>& client) {
    beast::flat_buffer message;
    client.read(message);
    return beast::buffers_to_string(message.data());
}

} // namespace

TEST(HttpServer, ServesRoutesAsynchronouslyAndRejectsOversizedBodies) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto status = std::make_shared<skai::RuntimeStatus>();
    status->set_running(true);
    auto api = std::make_shared<skai::ApiState>();
    skai::web::HttpServer server(logger, status, api);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    ASSERT_TRUE(server.initialize(config)) << logs.str();
    ASSERT_NE(server.port(), 0);
    ASSERT_TRUE(server.start());
    status->update_video(29.9);
    status->update_detector(18.4, 43.1);

    const auto health = request(server.port(),
                                {http::verb::get, "/health", 11});
    EXPECT_EQ(health.result(), http::status::ok);

    asio::io_context stalled_context;
    tcp::socket stalled(stalled_context);
    stalled.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    asio::write(stalled, asio::buffer("GET /health HTTP/1.1\r\n"));
    const auto concurrent = request(server.port(),
                                    {http::verb::get, "/api/v1/status", 11});
    EXPECT_EQ(concurrent.result(), http::status::ok);
    EXPECT_NE(concurrent.body().find("\"fps\":29.9"), std::string::npos);
    EXPECT_NE(concurrent.body().find("\"last_inference_ms\":43.1"),
              std::string::npos);
    const auto config_response = request(
        server.port(), {http::verb::get, "/api/v1/config", 11});
    EXPECT_EQ(config_response.result(), http::status::ok);
    const auto disable_response = request(
        server.port(), {http::verb::post, "/api/v1/detector/disable", 11});
    EXPECT_EQ(disable_response.result(), http::status::ok);
    EXPECT_FALSE(api->detector_enabled());
    beast::error_code ignored;
    stalled.close(ignored);

    http::request<http::string_body> oversized{http::verb::post, "/health", 11};
    oversized.body().assign(70 * 1024, 'x');
    oversized.prepare_payload();
    const auto rejected = request(server.port(), std::move(oversized));
    EXPECT_EQ(rejected.result(), http::status::payload_too_large);

    http::request<http::string_body> large_header{http::verb::get, "/health", 11};
    large_header.set("X-Large", std::string(17 * 1024, 'x'));
    const auto header_rejected = request(server.port(), std::move(large_header));
    EXPECT_EQ(header_rejected.result(),
              http::status::request_header_fields_too_large);

    server.stop();
    server.wait();
}

TEST(HttpServer, GracefulStopCancelsIncompleteHttpAndWebSocketHandshakes) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::web::HttpServer server(logger);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    ASSERT_TRUE(server.initialize(config));
    ASSERT_TRUE(server.start());

    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    asio::write(socket, asio::buffer("GET /health HTTP/1.1\r\n"));
    const auto start = std::chrono::steady_clock::now();
    server.stop();
    server.wait();
    EXPECT_LT(std::chrono::steady_clock::now() - start,
              std::chrono::seconds(1));

    for (int cycle = 0; cycle < 25; ++cycle) {
        ASSERT_TRUE(server.initialize(config));
        ASSERT_TRUE(server.start());
        tcp::socket racing_socket(context);
        racing_socket.connect({asio::ip::make_address("127.0.0.1"),
                               server.port()});
        server.stop();
        racing_socket.close();
        server.wait();
    }
    ASSERT_TRUE(server.initialize(config));
    ASSERT_TRUE(server.start());
    tcp::socket pending(context);
    pending.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    asio::write(pending, asio::buffer(
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"));
    server.stop();
    server.wait();
}

TEST(HttpServer, WebSocketBroadcastsEventsToMultipleClients) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto status = std::make_shared<skai::RuntimeStatus>();
    auto api = std::make_shared<skai::ApiState>();
    auto events = std::make_shared<skai::EventChannel>();
    logger.set_error_sink([events](const std::string& module,
                                   const std::string& message) {
        events->publish(skai::EventType::SystemError,
                        skai::make_system_error_data(module, message));
    });
    skai::web::HttpServer server(logger, status, api, events);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    ASSERT_TRUE(server.initialize(config));
    ASSERT_TRUE(server.start());

    asio::io_context first_context;
    websocket::stream<tcp::socket> first(first_context);
    connect_websocket(first, server.port());
    asio::io_context second_context;
    websocket::stream<tcp::socket> second(second_context);
    connect_websocket(second, server.port());

    for (auto* client : {&first, &second}) {
        EXPECT_NE(read_websocket(*client).find("\"type\":\"status\""),
                  std::string::npos);
        EXPECT_NE(read_websocket(*client).find("\"type\":\"gps\""),
                  std::string::npos);
    }

    events->publish(skai::EventType::Alert,
                    "{\"class\":\"person\",\"confidence\":0.91}");
    for (auto* client : {&first, &second}) {
        const auto json = read_websocket(*client);
        EXPECT_NE(json.find("\"type\":\"alert\""), std::string::npos);
        EXPECT_NE(json.find("\"class\":\"person\""), std::string::npos);
    }

    logger.log(skai::LogLevel::Error, "detector", "inference failed");
    for (auto* client : {&first, &second}) {
        const auto json = read_websocket(*client);
        EXPECT_NE(json.find("\"type\":\"system_error\""), std::string::npos);
        EXPECT_NE(json.find("\"module\":\"detector\""), std::string::npos);
        EXPECT_NE(json.find("\"message\":\"inference failed\""),
                  std::string::npos);
    }
    for (int update = 0; update < 6; ++update) {
        for (auto* client : {&first, &second}) {
            EXPECT_NE(read_websocket(*client).find("\"type\":\"status\""),
                      std::string::npos);
        }
    }

    first.close(websocket::close_code::normal);
    second.close(websocket::close_code::normal);
    server.stop();
    server.wait();
}

TEST(HttpServer, ShutdownClosesConnectedWebSocketPromptly) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::web::HttpServer server(logger);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    ASSERT_TRUE(server.initialize(config));
    ASSERT_TRUE(server.start());

    asio::io_context context;
    websocket::stream<tcp::socket> client(context);
    connect_websocket(client, server.port());
    read_websocket(client);
    read_websocket(client);

    auto close_result = std::async(std::launch::async, [&client] {
        beast::flat_buffer closed;
        beast::error_code error;
        client.read(closed, error);
        return error;
    });
    const auto start = std::chrono::steady_clock::now();
    server.stop();
    server.wait();
    EXPECT_LT(std::chrono::steady_clock::now() - start,
              std::chrono::seconds(1));
    EXPECT_EQ(close_result.get(), websocket::error::closed);
}
