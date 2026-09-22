#include "skai/web/http_server.hpp"
#include "skai/events.hpp"
#include "skai/gps/gps_source.hpp"
#include "skai/gps/gps_state.hpp"
#include "skai/status.hpp"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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

class TemporaryWebRoot {
public:
    TemporaryWebRoot() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("skai-web-test-" + suffix);
        root = base / "public";
        outside = base / "outside.txt";
        std::filesystem::create_directories(root / "assets");
        write(root / "index.html", "<!doctype html><title>SKAI Edge</title>");
        write(root / "app.js", "document.body.dataset.ready = 'true';");
        write(root / "style.css", "body { color: #fff; }");
        write(outside, "private");
        std::filesystem::create_symlink(outside, root / "escape.txt");
    }

    ~TemporaryWebRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
    }

    TemporaryWebRoot(const TemporaryWebRoot&) = delete;
    TemporaryWebRoot& operator=(const TemporaryWebRoot&) = delete;

    std::filesystem::path base;
    std::filesystem::path root;
    std::filesystem::path outside;

private:
    static void write(const std::filesystem::path& path,
                      const std::string& contents) {
        std::ofstream file(path, std::ios::binary);
        file << contents;
    }
};

http::response<http::string_body> request(unsigned short port,
                                          http::request<http::string_body> value) {
    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    const bool head = value.method() == http::verb::head;
    http::write(socket, value);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.skip(head);
    http::read(socket, buffer, parser);
    return parser.release();
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

TEST(HttpServer, ServesStaticFrontendWithContentTypes) {
    TemporaryWebRoot files;
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::web::HttpServer server(logger);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    config.web.root = files.root.string();
    ASSERT_TRUE(server.initialize(config)) << logs.str();
    ASSERT_TRUE(std::filesystem::remove(files.root / "app.js"));
    ASSERT_TRUE(server.start());

    const auto index = request(server.port(), {http::verb::get, "/", 11});
    EXPECT_EQ(index.result(), http::status::ok);
    EXPECT_EQ(index[http::field::content_type], "text/html; charset=utf-8");
    EXPECT_NE(index.body().find("SKAI Edge"), std::string::npos);

    const auto script = request(server.port(), {http::verb::get, "/app.js", 11});
    EXPECT_EQ(script.result(), http::status::ok);
    EXPECT_EQ(script[http::field::content_type],
              "text/javascript; charset=utf-8");
    const auto script_head = request(
        server.port(), {http::verb::head, "/app.js", 11});
    EXPECT_EQ(script_head.result(), http::status::ok);
    EXPECT_TRUE(script_head.body().empty());
    EXPECT_EQ(script_head[http::field::content_length],
              std::to_string(script.body().size()));
    const auto style = request(server.port(), {http::verb::get, "/style.css", 11});
    EXPECT_EQ(style.result(), http::status::ok);
    EXPECT_EQ(style[http::field::content_type], "text/css; charset=utf-8");

    server.stop();
    server.wait();
}

TEST(HttpServer, RejectsUnboundedStaticAssetsDuringInitialization) {
    TemporaryWebRoot files;
    {
        std::ofstream script(files.root / "app.js", std::ios::binary);
        script.seekp(1024 * 1024);
        script.put('x');
    }
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::web::HttpServer server(logger);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    config.web.root = files.root.string();

    EXPECT_FALSE(server.initialize(config));
    EXPECT_NE(logs.str().find("exceeds 1 MiB"), std::string::npos);
}

TEST(HttpServer, ConfinesStaticRequestsToConfiguredRoot) {
    TemporaryWebRoot files;
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::web::HttpServer server(logger);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    config.web.root = files.root.string();
    ASSERT_TRUE(server.initialize(config)) << logs.str();
    ASSERT_TRUE(server.start());

    EXPECT_EQ(request(server.port(), {http::verb::get, "/missing.txt", 11}).result(),
              http::status::not_found);
    for (const auto* target : {"/../config/config.example.yaml",
                               "/%2e%2e/%2e%2e/etc/passwd",
                               "/..%2f..%2fetc/passwd"}) {
        EXPECT_EQ(request(server.port(), {http::verb::get, target, 11}).result(),
                  http::status::forbidden) << target;
    }
    EXPECT_EQ(request(server.port(), {http::verb::get, "/escape.txt", 11}).result(),
              http::status::forbidden);
    EXPECT_EQ(request(server.port(), {http::verb::get, "/assets", 11}).result(),
              http::status::not_found);

    server.stop();
    server.wait();
}

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
    const auto health_with_query = request(
        server.port(), {http::verb::get, "/health?probe=readiness", 11});
    EXPECT_EQ(health_with_query.result(), http::status::ok);

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
    auto gps_state = std::make_shared<skai::GpsState>();
    gps_state->update(skai::GpsSource(skai::GpsConfig{}).latest());
    auto api = std::make_shared<skai::ApiState>(nullptr, gps_state);
    auto events = std::make_shared<skai::EventChannel>();
    auto webrtc = std::make_shared<skai::WebRtcManager>(logger);
    logger.set_error_sink([events](const std::string& module,
                                   const std::string& message) {
        events->publish(skai::EventType::SystemError,
                        skai::make_system_error_data(module, message));
    });
    skai::web::HttpServer server(logger, status, api, events, nullptr, nullptr,
                                 webrtc);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    config.webrtc.enabled = false;
    config.webrtc.host_interfaces.clear();
    ASSERT_TRUE(server.initialize(config));
    ASSERT_TRUE(server.start());

    asio::io_context first_context;
    websocket::stream<tcp::socket> first(first_context);
    connect_websocket(first, server.port());
    asio::io_context second_context;
    websocket::stream<tcp::socket> second(second_context);
    connect_websocket(second, server.port());

    for (auto* client : {&first, &second}) {
        const auto status_event = read_websocket(*client);
        EXPECT_NE(status_event.find("\"type\":\"status\""), std::string::npos);
        EXPECT_NE(status_event.find("\"webrtc\":{\"enabled\":false"),
                  std::string::npos);
        EXPECT_NE(status_event.find("\"lan_only\":true"), std::string::npos);
        EXPECT_NE(status_event.find("\"signaling_errors\":0"), std::string::npos);
        EXPECT_NE(status_event.find("\"recently_closed\":[]"), std::string::npos);
        const auto gps = read_websocket(*client);
        EXPECT_NE(gps.find("\"type\":\"gps\""), std::string::npos);
        EXPECT_NE(gps.find("\"source\":\"fixed\""), std::string::npos);
        EXPECT_NE(gps.find("\"valid\":true"), std::string::npos);
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
    const auto disable = request(
        server.port(), {http::verb::post, "/api/v1/detector/disable", 11});
    ASSERT_EQ(disable.result(), http::status::ok);
    for (auto* client : {&first, &second}) {
        const auto json = read_websocket(*client);
        EXPECT_NE(json.find("\"type\":\"status\""), std::string::npos);
        EXPECT_NE(json.find("\"detector_enabled\":false"), std::string::npos);
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
