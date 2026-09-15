#include "skai/config.hpp"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

namespace skai {
namespace {

void check_keys(const YAML::Node& node, const std::string& path,
                std::initializer_list<const char*> allowed) {
    if (!node.IsMap()) throw std::invalid_argument(path + " must be a mapping");
    std::set<std::string> seen;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) throw std::invalid_argument(path + " has a non-string key");
        const auto key = entry.first.as<std::string>();
        const auto field = path.empty() ? key : path + "." + key;
        if (!seen.insert(key).second) throw std::invalid_argument(field + " is duplicated");
        bool known = false;
        for (const auto* allowed_key : allowed) {
            if (key == allowed_key) known = true;
        }
        if (!known) throw std::invalid_argument("unknown configuration key: " + field);
    }
}

template <typename T>
void read_scalar(const YAML::Node& section, const char* key, const std::string& path,
                 T& value) {
    const auto field = section[key];
    if (!field) return;
    if (!field.IsScalar()) throw std::invalid_argument(path + "." + key + " must be a scalar");
    try {
        value = field.as<T>();
    } catch (const YAML::Exception&) {
        throw std::invalid_argument(path + "." + key + " has an invalid value");
    }
}

void check_range(bool valid, const std::string& field, const std::string& requirement) {
    if (!valid) throw std::invalid_argument(field + " " + requirement);
}

void validate(const Config& config) {
    const auto& video = config.video;
    const auto& url = video.rtsp_url;
    const auto authority_start = std::string("rtsp://").size();
    const auto authority_end = url.size() >= authority_start
                                   ? url.find_first_of("/?#", authority_start)
                                   : std::string::npos;
    const auto authority = url.size() >= authority_start
                               ? url.substr(authority_start, authority_end - authority_start)
                               : std::string{};
    const auto host = authority.substr(authority.find('@') == std::string::npos
                                           ? 0 : authority.rfind('@') + 1);
    check_range(url.rfind("rtsp://", 0) == 0 && !host.empty() &&
                    host.find_first_of(" \t\r\n") == std::string::npos &&
                    url.find_first_of(" \t\r\n") == std::string::npos,
                "video.rtsp_url", "must be a single rtsp:// URL with a host");
    check_range(video.transport == "tcp" || video.transport == "udp",
                "video.transport", "must be tcp or udp");
    check_range(video.latency_ms >= 0 && video.latency_ms <= 10000,
                "video.latency_ms", "must be between 0 and 10000");
    check_range(video.reconnect_delay_ms >= 1 && video.reconnect_delay_ms <= 60000,
                "video.reconnect_delay_ms", "must be between 1 and 60000");
    check_range(video.stall_timeout_ms >= 1 && video.stall_timeout_ms <= 60000,
                "video.stall_timeout_ms", "must be between 1 and 60000");

    check_range(!config.detector.engine.empty(), "detector.engine", "must not be empty");
    check_range(std::isfinite(config.detector.confidence) &&
                    config.detector.confidence >= 0 && config.detector.confidence <= 1,
                "detector.confidence", "must be between 0 and 1");
    check_range(std::isfinite(config.detector.nms) && config.detector.nms >= 0 &&
                    config.detector.nms <= 1,
                "detector.nms", "must be between 0 and 1");
    check_range(!config.web.bind.empty(), "web.bind", "must not be empty");
    check_range(config.web.port >= 1 && config.web.port <= 65535,
                "web.port", "must be between 1 and 65535");
    check_range(!config.recording.directory.empty(), "recording.directory", "must not be empty");
    check_range(config.recording.segment_seconds >= 1,
                "recording.segment_seconds", "must be positive");

    check_range(config.gps.source == "fixed", "gps.source", "must be fixed");
    check_range(std::isfinite(config.gps.latitude) && std::abs(config.gps.latitude) <= 90,
                "gps.latitude", "must be between -90 and 90");
    check_range(std::isfinite(config.gps.longitude) && std::abs(config.gps.longitude) <= 180,
                "gps.longitude", "must be between -180 and 180");
    check_range(std::isfinite(config.gps.altitude_m), "gps.altitude_m", "must be finite");
    check_range(config.webrtc.max_peers >= 1 && config.webrtc.max_peers <= 100,
                "webrtc.max_peers", "must be between 1 and 100");
    check_range(config.webrtc.ice_log_verbosity >= 0 && config.webrtc.ice_log_verbosity <= 5,
                "webrtc.ice_log_verbosity", "must be between 0 and 5");
    for (const auto& interface : config.webrtc.host_interfaces) {
        check_range(!interface.empty() && interface.find_first_of(" /\t\r\n") == std::string::npos,
                    "webrtc.host_interfaces", "must contain interface names without spaces or slashes");
    }
}

Config parse(const YAML::Node& root) {
    check_keys(root, "", {"video", "detector", "web", "recording", "gps", "webrtc", "logging"});
    if (!root["video"] || !root["video"].IsMap() || !root["video"]["rtsp_url"]) {
        throw std::invalid_argument("video.rtsp_url is required");
    }
    Config config;
    const auto video = root["video"];
    check_keys(video, "video", {"rtsp_url", "transport", "latency_ms", "reconnect_delay_ms",
                                "stall_timeout_ms"});
    read_scalar(video, "rtsp_url", "video", config.video.rtsp_url);
    read_scalar(video, "transport", "video", config.video.transport);
    read_scalar(video, "latency_ms", "video", config.video.latency_ms);
    read_scalar(video, "reconnect_delay_ms", "video", config.video.reconnect_delay_ms);
    read_scalar(video, "stall_timeout_ms", "video", config.video.stall_timeout_ms);

    if (const auto section = root["detector"]) {
        check_keys(section, "detector", {"engine", "confidence", "nms"});
        read_scalar(section, "engine", "detector", config.detector.engine);
        read_scalar(section, "confidence", "detector", config.detector.confidence);
        read_scalar(section, "nms", "detector", config.detector.nms);
    }
    if (const auto section = root["web"]) {
        check_keys(section, "web", {"bind", "port"});
        read_scalar(section, "bind", "web", config.web.bind);
        read_scalar(section, "port", "web", config.web.port);
    }
    if (const auto section = root["recording"]) {
        check_keys(section, "recording", {"enabled", "directory", "segment_seconds"});
        read_scalar(section, "enabled", "recording", config.recording.enabled);
        read_scalar(section, "directory", "recording", config.recording.directory);
        read_scalar(section, "segment_seconds", "recording", config.recording.segment_seconds);
    }
    if (const auto section = root["gps"]) {
        check_keys(section, "gps", {"enabled", "source", "latitude", "longitude", "altitude_m"});
        read_scalar(section, "enabled", "gps", config.gps.enabled);
        read_scalar(section, "source", "gps", config.gps.source);
        read_scalar(section, "latitude", "gps", config.gps.latitude);
        read_scalar(section, "longitude", "gps", config.gps.longitude);
        read_scalar(section, "altitude_m", "gps", config.gps.altitude_m);
    }
    if (const auto section = root["webrtc"]) {
        check_keys(section, "webrtc", {"enabled", "max_peers", "host_interfaces",
                                       "ice_log_verbosity"});
        read_scalar(section, "enabled", "webrtc", config.webrtc.enabled);
        read_scalar(section, "max_peers", "webrtc", config.webrtc.max_peers);
        read_scalar(section, "ice_log_verbosity", "webrtc", config.webrtc.ice_log_verbosity);
        if (const auto interfaces = section["host_interfaces"]) {
            if (!interfaces.IsSequence()) {
                throw std::invalid_argument("webrtc.host_interfaces must be a sequence");
            }
            config.webrtc.host_interfaces.clear();
            for (const auto& item : interfaces) {
                if (!item.IsScalar()) {
                    throw std::invalid_argument("webrtc.host_interfaces must contain strings");
                }
                config.webrtc.host_interfaces.push_back(item.as<std::string>());
            }
        }
    }
    if (const auto section = root["logging"]) {
        check_keys(section, "logging", {"level"});
        std::string level = "info";
        read_scalar(section, "level", "logging", level);
        if (level == "trace") config.logging.level = LogLevel::Trace;
        else if (level == "debug") config.logging.level = LogLevel::Debug;
        else if (level == "info") config.logging.level = LogLevel::Info;
        else if (level == "warning") config.logging.level = LogLevel::Warning;
        else if (level == "error") config.logging.level = LogLevel::Error;
        else throw std::invalid_argument("logging.level must be trace, debug, info, warning, or error");
    }
    validate(config);
    return config;
}

} // namespace

ConfigResult parse_config(const std::string& text) {
    try {
        return {true, parse(YAML::Load(text)), {}};
    } catch (const std::exception& error) {
        return {false, {}, error.what()};
    }
}

ConfigResult load_config(const std::string& path) {
    std::ifstream input(path);
    if (!input) return {false, {}, "could not open configuration file: " + path};
    const std::string text(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad()) return {false, {}, "could not read configuration file: " + path};
    auto result = parse_config(text);
    if (!result.ok) result.error = path + ": " + result.error;
    return result;
}

} // namespace skai
