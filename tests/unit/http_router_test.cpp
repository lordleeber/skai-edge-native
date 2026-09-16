#include "skai/web/router.hpp"

#include <gtest/gtest.h>

#include <boost/beast/http.hpp>

#include <string>

namespace http = boost::beast::http;

TEST(HttpRouter, ServesHealthAndRuntimeStatusJson) {
    skai::web::StatusSnapshot status;
    status.status = "running";
    status.uptime_s = 812;
    status.video_fps = 29.9;
    status.detector_fps = 18.4;
    status.last_inference_ms = 43.1;

    const auto health = skai::web::route_request(
        {http::verb::get, "/health", 11}, status);
    EXPECT_EQ(health.result(), http::status::ok);
    EXPECT_EQ(health[http::field::content_type], "application/json");
    EXPECT_EQ(health.body(), "{\"status\":\"ok\"}\n");

    const auto response = skai::web::route_request(
        {http::verb::get, "/api/v1/status", 11}, status);
    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_NE(response.body().find("\"status\":\"running\""), std::string::npos);
    EXPECT_NE(response.body().find("\"uptime_s\":812"), std::string::npos);
    EXPECT_NE(response.body().find("\"fps\":29.9"), std::string::npos);
    EXPECT_NE(response.body().find("\"last_inference_ms\":43.1"),
              std::string::npos);
}

TEST(HttpRouter, RejectsUnknownRoutesAndUnsupportedMethods) {
    const skai::web::StatusSnapshot status;
    const auto missing = skai::web::route_request(
        {http::verb::get, "/api/v1/config", 11}, status);
    EXPECT_EQ(missing.result(), http::status::not_found);

    const auto method = skai::web::route_request(
        {http::verb::post, "/health", 11}, status);
    EXPECT_EQ(method.result(), http::status::method_not_allowed);
    EXPECT_EQ(method[http::field::allow], "GET");
}
