#include "skai/web/router.hpp"
#include "skai/api_state.hpp"

#include <gtest/gtest.h>

#include <boost/beast/http.hpp>

#include <string>

namespace http = boost::beast::http;

TEST(HttpRouter, ServesHealthAndRuntimeStatusJson) {
    skai::ApiState api;
    skai::web::StatusSnapshot status;
    status.status = "running";
    status.uptime_s = 812;
    status.video_fps = 29.9;
    status.detector_fps = 18.4;
    status.last_inference_ms = 43.1;

    const auto health = skai::web::route_request(
        {http::verb::get, "/health", 11}, status, api);
    EXPECT_EQ(health.result(), http::status::ok);
    EXPECT_EQ(health[http::field::content_type], "application/json");
    EXPECT_EQ(health.body(), "{\"status\":\"ok\"}\n");

    const auto response = skai::web::route_request(
        {http::verb::get, "/api/v1/status", 11}, status, api);
    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_NE(response.body().find("\"status\":\"running\""), std::string::npos);
    EXPECT_NE(response.body().find("\"uptime_s\":812"), std::string::npos);
    EXPECT_NE(response.body().find("\"fps\":29.9"), std::string::npos);
    EXPECT_NE(response.body().find("\"last_inference_ms\":43.1"),
              std::string::npos);
}

TEST(HttpRouter, RejectsUnknownRoutesAndUnsupportedMethods) {
    skai::ApiState api;
    const skai::web::StatusSnapshot status;
    const auto missing = skai::web::route_request(
        {http::verb::get, "/missing", 11}, status, api);
    EXPECT_EQ(missing.result(), http::status::not_found);

    const auto method = skai::web::route_request(
        {http::verb::post, "/health", 11}, status, api);
    EXPECT_EQ(method.result(), http::status::method_not_allowed);
    EXPECT_EQ(method[http::field::allow], "GET");

    const auto unavailable = skai::web::route_request(
        {http::verb::get, "/api/v1/status", 11}, status, api);
    EXPECT_NE(unavailable.body().find("\"fps\":null"), std::string::npos);
    EXPECT_NE(unavailable.body().find("\"last_inference_ms\":null"),
              std::string::npos);
}

TEST(HttpRouter, ServesExplicitConfigDetectionAndGpsDtos) {
    skai::ApiState api;
    skai::Config config;
    config.video.transport = "udp";
    config.video.username = "secret-user";
    config.video.password = "secret-password";
    config.detector.confidence = 0.42;
    config.gps.latitude = 25.033964;
    api.configure(config);
    skai::RuntimeStatusSnapshot status;

    const auto config_response = skai::web::route_request(
        {http::verb::get, "/api/v1/config", 11}, status, api);
    EXPECT_EQ(config_response.result(), http::status::ok);
    EXPECT_NE(config_response.body().find("\"transport\":\"udp\""),
              std::string::npos);
    EXPECT_NE(config_response.body().find("\"confidence\":0.42"),
              std::string::npos);
    EXPECT_EQ(config_response.body().find("secret-user"), std::string::npos);
    EXPECT_EQ(config_response.body().find("secret-password"), std::string::npos);

    const auto gps = skai::web::route_request(
        {http::verb::get, "/api/v1/gps", 11}, status, api);
    EXPECT_EQ(gps.result(), http::status::ok);
    EXPECT_NE(gps.body().find("\"source\":\"fixed\""), std::string::npos);
    EXPECT_NE(gps.body().find("25.034"), std::string::npos);

    const auto no_detections = skai::web::route_request(
        {http::verb::get, "/api/v1/detections/latest", 11}, status, api);
    EXPECT_NE(no_detections.body().find("\"available\":false"),
              std::string::npos);

    api.publish_detections(17, {{0, "person", 0.91f, 1, 2, 3, 4}});
    const auto detections = skai::web::route_request(
        {http::verb::get, "/api/v1/detections/latest", 11}, status, api);
    EXPECT_EQ(detections.result(), http::status::ok);
    EXPECT_NE(detections.body().find("\"frame_sequence\":17"),
              std::string::npos);
    EXPECT_NE(detections.body().find("\"class_name\":\"person\""),
              std::string::npos);
}

TEST(HttpRouter, ControlsDetectorAndMakesDeferredSubsystemsExplicit) {
    skai::ApiState api;
    api.configure(skai::Config{});
    skai::RuntimeStatusSnapshot status;

    const auto disabled = skai::web::route_request(
        {http::verb::post, "/api/v1/detector/disable", 11}, status, api);
    EXPECT_EQ(disabled.result(), http::status::ok);
    EXPECT_FALSE(api.detector_enabled());
    const auto enabled = skai::web::route_request(
        {http::verb::post, "/api/v1/detector/enable", 11}, status, api);
    EXPECT_EQ(enabled.result(), http::status::ok);
    EXPECT_TRUE(api.detector_enabled());

    for (const auto* target : {"/api/v1/alerts", "/api/v1/alerts/a-1",
                               "/api/v1/alerts?limit=100&class=person",
                               "/api/v1/alerts?from=1000&to=2000",
                               "/api/v1/recordings"}) {
        const auto response = skai::web::route_request(
            {http::verb::get, target, 11}, status, api);
        EXPECT_EQ(response.result(), http::status::service_unavailable) << target;
        EXPECT_NE(response.body().find("\"available\":false"),
                  std::string::npos) << target;
    }
    for (const auto* target : {"/api/v1/recording/start",
                               "/api/v1/recording/stop"}) {
        const auto recording = skai::web::route_request(
            {http::verb::post, target, 11}, status, api);
        EXPECT_EQ(recording.result(), http::status::not_implemented);
    }
    api.set_detector_supported(false);
    const auto unsupported = skai::web::route_request(
        {http::verb::post, "/api/v1/detector/enable", 11}, status, api);
    EXPECT_EQ(unsupported.result(), http::status::service_unavailable);
}
