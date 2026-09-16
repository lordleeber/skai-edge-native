#pragma once

#include "skai/logging.hpp"

#include <string>
#include <vector>

namespace skai {

struct VideoConfig {
    std::string rtsp_url = "rtsp://127.0.0.1/stream";
    std::string username;
    std::string password;
    std::string transport = "tcp";
    int latency_ms = 100;
    int reconnect_delay_ms = 1000;
    int max_reconnect_delay_ms = 10000;
    int stall_timeout_ms = 3000;
    int first_frame_timeout_ms = 15000;
};

struct DetectorConfig {
    std::string engine = "models/yolo11s.engine";
    double confidence = 0.35;
    double nms = 0.45;
    bool annotate = true;
};

struct WebConfig {
    std::string bind = "0.0.0.0";
    int port = 8080;
};

struct RecordingConfig {
    bool enabled = true;
    std::string directory = "recordings";
    int segment_seconds = 300;
};

struct GpsConfig {
    bool enabled = true;
    std::string source = "fixed";
    double latitude = 25.033964;
    double longitude = 121.564468;
    double altitude_m = 10.0;
};

struct WebrtcConfig {
    bool enabled = true;
    int max_peers = 2;
    std::vector<std::string> host_interfaces = {"eth0", "wlan0"};
    int ice_log_verbosity = 1;
};

struct LoggingConfig {
    LogLevel level = LogLevel::Info;
};

struct Config {
    VideoConfig video;
    DetectorConfig detector;
    WebConfig web;
    RecordingConfig recording;
    GpsConfig gps;
    WebrtcConfig webrtc;
    LoggingConfig logging;
};

struct ConfigResult {
    bool ok;
    Config config;
    std::string error;
};

ConfigResult parse_config(const std::string& text);
ConfigResult load_config(const std::string& path);
bool validate_video_config(const VideoConfig& video, std::string& error);

} // namespace skai
