#include "skai/config.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

constexpr const char* valid_config = R"(
video:
  rtsp_url: rtsp://100.69.117.102:8554/test
  transport: tcp
  latency_ms: 100
detector:
  confidence: 0.35
  nms: 0.45
web:
  port: 8080
gps:
  latitude: 25.033964
  longitude: 121.564468
webrtc:
  host_interfaces: [eth0, wlan0]
logging:
  level: debug
)";

} // namespace

TEST(Config, ParsesValidSettings) {
    const auto result = skai::parse_config(valid_config);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.config.video.rtsp_url, "rtsp://100.69.117.102:8554/test");
    EXPECT_EQ(result.config.video.latency_ms, 100);
    EXPECT_EQ(result.config.web.port, 8080);
    EXPECT_EQ(result.config.webrtc.host_interfaces.size(), 2U);
    EXPECT_EQ(result.config.logging.level, skai::LogLevel::Debug);
}

TEST(Config, RejectsMissingRtspUrl) {
    const auto result = skai::parse_config("video: {transport: tcp}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("video.rtsp_url"), std::string::npos);
}

TEST(Config, RejectsInvalidRtspUrl) {
    const auto result = skai::parse_config("video: {rtsp_url: file:///tmp/video.mp4}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("video.rtsp_url"), std::string::npos);
}

TEST(Config, RejectsEmptyRtspUrlWithFieldName) {
    const auto result = skai::parse_config("video: {rtsp_url: ''}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("video.rtsp_url"), std::string::npos);
}

TEST(Config, RejectsInvalidGpsCoordinate) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\ngps: {latitude: 91}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("gps.latitude"), std::string::npos);
}

TEST(Config, RejectsUnknownLoggingLevel) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\nlogging: {level: loud}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("logging.level"), std::string::npos);
}

TEST(Config, RejectsOutOfRangeValues) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\nweb: {port: 70000}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("web.port"), std::string::npos);
}

TEST(Config, RejectsIncorrectTypes) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream', latency_ms: nope}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("video.latency_ms"), std::string::npos);
}

TEST(Config, RejectsUnknownKeys) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream', file_path: video.mp4}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("video.file_path"), std::string::npos);
}

TEST(Config, RejectsMalformedYaml) {
    const auto result = skai::parse_config("video: [\n");
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST(Config, ReportsMissingFile) {
    const auto result = skai::load_config("/no/such/skai-config.yaml");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("/no/such/skai-config.yaml"), std::string::npos);
}
