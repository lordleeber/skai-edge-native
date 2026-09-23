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
  root: /srv/skai-edge/web
storage:
  database_path: /tmp/skai-edge-test.db
  alert_directory: /tmp/skai-edge-alerts
gps:
  latitude: 25.033964
  longitude: 121.564468
webrtc:
  connection_timeout_ms: 12000
  media_queue_capacity: 12
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
    EXPECT_EQ(result.config.web.root, "/srv/skai-edge/web");
    EXPECT_EQ(result.config.storage.database_path, "/tmp/skai-edge-test.db");
    EXPECT_EQ(result.config.storage.alert_directory, "/tmp/skai-edge-alerts");
    EXPECT_EQ(result.config.webrtc.host_interfaces.size(), 2U);
    EXPECT_EQ(result.config.webrtc.connection_timeout_ms, 12000);
    EXPECT_EQ(result.config.webrtc.media_queue_capacity, 12);
    EXPECT_EQ(result.config.logging.level, skai::LogLevel::Debug);
}

TEST(Config, CanDisableDetectorAnnotation) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\n"
        "detector: {annotate: false}\n");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.config.detector.annotate);
}

TEST(Config, ParsesAlertRulesWithOptionalNormalizedRoi) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\n"
        "alerts:\n"
        "  - class: person\n"
        "    confidence: 0.7\n"
        "    consecutive_frames: 3\n"
        "    cooldown_seconds: 10\n"
        "    roi: {x1: 0.1, y1: 0.2, x2: 0.8, y2: 0.9}\n");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.config.alerts.size(), 1U);
    const auto& rule = result.config.alerts[0];
    EXPECT_EQ(rule.class_name, "person");
    EXPECT_DOUBLE_EQ(rule.confidence, 0.7);
    EXPECT_EQ(rule.consecutive_frames, 3);
    EXPECT_EQ(rule.cooldown_seconds, 10);
    ASSERT_TRUE(rule.roi.has_value());
    EXPECT_DOUBLE_EQ(rule.roi->x1, 0.1);
    EXPECT_DOUBLE_EQ(rule.roi->y2, 0.9);
}

TEST(Config, RejectsInvalidAlertRules) {
    for (const auto* rule : {
             "{class: '', confidence: 0.7}",
             "{class: person, confidence: 1.1}",
             "{class: person, consecutive_frames: 0}",
             "{class: person, cooldown_seconds: -1}",
             "{class: person, roi: {x1: 0, y1: 0, x2: 1}}",
             "{class: person, roi: {x1: 0.8, y1: 0, x2: 0.2, y2: 1}}",
             "{class: person, roi: {x1: -0.1, y1: 0, x2: 1, y2: 1}}"}) {
        const auto result = skai::parse_config(
            "video: {rtsp_url: 'rtsp://camera/stream'}\nalerts: [" +
            std::string(rule) + "]\n");
        EXPECT_FALSE(result.ok) << rule;
        EXPECT_NE(result.error.find("alerts[0]"), std::string::npos) << rule;
    }
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

TEST(Config, RejectsMalformedRtspAuthorities) {
    for (const auto* url : {"rtsp://:8554/test", "rtsp://camera:/test",
                            "rtsp://camera:not-a-port/test", "rtsp://camera:0/test",
                            "rtsp://camera:65536/test"}) {
        const auto result = skai::parse_config(
            "video: {rtsp_url: '" + std::string(url) + "'}\n");
        EXPECT_FALSE(result.ok) << url;
        EXPECT_NE(result.error.find("video.rtsp_url"), std::string::npos) << url;
    }
}

TEST(Config, AcceptsRtspUrlWithIpv6HostAndPort) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://[::1]:8554/test'}\n");
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(Config, AcceptsRtspUrlWithEmbeddedCredentials) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://viewer:password@camera.local:8554/live'}\n");
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(Config, AcceptsConfiguredRtspCredentials) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera.local/live', username: viewer, password: secret}\n");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.config.video.username, "viewer");
    EXPECT_EQ(result.config.video.password, "secret");
}

TEST(Config, RejectsPasswordWithoutRtspUsername) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera.local/live', password: secret}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("video.username"), std::string::npos);
}

TEST(Config, RejectsEmptyEnabledWebrtcInterfaceWhitelist) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\n"
        "webrtc: {enabled: true, host_interfaces: []}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("webrtc.host_interfaces"), std::string::npos);
}

TEST(Config, AllowsEmptyDisabledWebrtcInterfaceWhitelist) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\n"
        "webrtc: {enabled: false, host_interfaces: []}\n");
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(Config, RejectsInvalidWebrtcHardeningLimits) {
    for (const auto* settings : {"connection_timeout_ms: 999",
                                 "connection_timeout_ms: 300001",
                                 "media_queue_capacity: 0",
                                 "media_queue_capacity: 121"}) {
        const auto result = skai::parse_config(
            "video: {rtsp_url: 'rtsp://camera/stream'}\nwebrtc: {" +
            std::string(settings) + "}\n");
        EXPECT_FALSE(result.ok) << settings;
        EXPECT_NE(result.error.find("webrtc."), std::string::npos) << settings;
    }
}

TEST(Config, RejectsInvalidGpsCoordinate) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\ngps: {latitude: 91}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("gps.latitude"), std::string::npos);
}

TEST(Config, RejectsInvalidGpsLongitudeAndSource) {
    const auto longitude = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\ngps: {longitude: -181}\n");
    EXPECT_FALSE(longitude.ok);
    EXPECT_NE(longitude.error.find("gps.longitude"), std::string::npos);

    const auto source = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\ngps: {source: serial}\n");
    EXPECT_FALSE(source.ok);
    EXPECT_NE(source.error.find("gps.source"), std::string::npos);
    EXPECT_NE(source.error.find("fixed"), std::string::npos);
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

TEST(Config, RejectsEmptyWebRoot) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\nweb: {root: ''}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("web.root"), std::string::npos);
}

TEST(Config, RejectsEmptyDatabasePath) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\nstorage: {database_path: ''}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("storage.database_path"), std::string::npos);
}

TEST(Config, RejectsEmptyAlertDirectory) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream'}\n"
        "storage: {alert_directory: ''}\n");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("storage.alert_directory"), std::string::npos);
}

TEST(Config, ValidatesReconnectBackoffCeiling) {
    const auto valid = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream', reconnect_delay_ms: 100, max_reconnect_delay_ms: 800}\n");
    ASSERT_TRUE(valid.ok) << valid.error;
    EXPECT_EQ(valid.config.video.max_reconnect_delay_ms, 800);
    const auto invalid = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream', reconnect_delay_ms: 800, max_reconnect_delay_ms: 100}\n");
    EXPECT_FALSE(invalid.ok);
    EXPECT_NE(invalid.error.find("video.max_reconnect_delay_ms"), std::string::npos);
}

TEST(Config, SeparatesFirstFrameTimeoutFromStallTimeout) {
    const auto result = skai::parse_config(
        "video: {rtsp_url: 'rtsp://camera/stream', first_frame_timeout_ms: 12000, stall_timeout_ms: 500}\n");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.config.video.first_frame_timeout_ms, 12000);
    EXPECT_EQ(result.config.video.stall_timeout_ms, 500);
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
