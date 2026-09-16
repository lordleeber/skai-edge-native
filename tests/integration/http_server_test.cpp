#include "skai/web/http_server.hpp"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <sstream>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
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

} // namespace

TEST(HttpServer, ServesRoutesAsynchronouslyAndRejectsOversizedBodies) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::web::HttpServer server(logger);
    skai::Config config;
    config.web.bind = "127.0.0.1";
    config.web.port = 0;
    ASSERT_TRUE(server.initialize(config)) << logs.str();
    ASSERT_NE(server.port(), 0);
    ASSERT_TRUE(server.start());

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

TEST(HttpServer, GracefulStopCancelsAnIncompleteRequest) {
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
}
