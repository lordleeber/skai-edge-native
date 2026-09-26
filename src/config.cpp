#include "skai/config.hpp"

#include <yaml-cpp/yaml.h>

#include <arpa/inet.h>

#include <charconv>
#include <cmath>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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

bool valid_port(const std::string& text) {
    if (text.empty()) return false;
    unsigned int port = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           port >= 1 && port <= 65535;
}

bool loopback_http_endpoint(const std::string& url) {
    const std::string prefix = "http://127.0.0.1:";
    if (url.rfind(prefix, 0) != 0) return false;
    const auto path = url.find('/', prefix.size());
    return path != std::string::npos &&
           valid_port(url.substr(prefix.size(), path - prefix.size()));
}

bool valid_hostname(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    if (host.find('.') != std::string::npos &&
        host.find_first_not_of("0123456789.") == std::string::npos) {
        in_addr address{};
        return inet_pton(AF_INET, host.c_str(), &address) == 1;
    }
    std::size_t start = 0;
    while (start < host.size()) {
        const auto end = host.find('.', start);
        const auto length = (end == std::string::npos ? host.size() : end) - start;
        if (length == 0 || length > 63 ||
            !std::isalnum(static_cast<unsigned char>(host[start])) ||
            !std::isalnum(static_cast<unsigned char>(host[start + length - 1]))) return false;
        for (std::size_t index = start; index < start + length; ++index) {
            const auto c = static_cast<unsigned char>(host[index]);
            if (!std::isalnum(c) && c != '-') return false;
        }
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

bool valid_rtsp_authority(const std::string& authority) {
    const auto at = authority.rfind('@');
    if (at != std::string::npos && (at == 0 || authority.find('@') != at)) return false;
    const auto host_port = authority.substr(at == std::string::npos ? 0 : at + 1);
    if (host_port.empty()) return false;

    if (host_port.front() == '[') {
        const auto close = host_port.find(']');
        if (close == std::string::npos) return false;
        const auto address_text = host_port.substr(1, close - 1);
        in6_addr address{};
        if (inet_pton(AF_INET6, address_text.c_str(), &address) != 1) return false;
        if (close + 1 == host_port.size()) return true;
        return host_port[close + 1] == ':' && valid_port(host_port.substr(close + 2));
    }

    const auto colon = host_port.find(':');
    const auto host = host_port.substr(0, colon);
    return valid_hostname(host) &&
           (colon == std::string::npos || valid_port(host_port.substr(colon + 1)));
}

void validate_video(const VideoConfig& video) {
    const auto& url = video.rtsp_url;
    const auto authority_start = std::string("rtsp://").size();
    const auto authority_end = url.size() >= authority_start
                                   ? url.find_first_of("/?#", authority_start)
                                   : std::string::npos;
    const auto authority = url.size() >= authority_start
                               ? url.substr(authority_start, authority_end - authority_start)
                               : std::string{};
    check_range(url.rfind("rtsp://", 0) == 0 && valid_rtsp_authority(authority) &&
                    url.find_first_of(" \t\r\n") == std::string::npos,
                "video.rtsp_url", "must be a rtsp:// URL with a valid host and optional port");
    check_range(video.transport == "tcp" || video.transport == "udp",
                "video.transport", "must be tcp or udp");
    check_range(video.password.empty() || !video.username.empty(),
                "video.username", "must be set when video.password is set");
    check_range(video.latency_ms >= 0 && video.latency_ms <= 10000,
                "video.latency_ms", "must be between 0 and 10000");
    check_range(video.reconnect_delay_ms >= 1 && video.reconnect_delay_ms <= 60000,
                "video.reconnect_delay_ms", "must be between 1 and 60000");
    check_range(video.max_reconnect_delay_ms >= video.reconnect_delay_ms &&
                    video.max_reconnect_delay_ms <= 60000,
                "video.max_reconnect_delay_ms",
                "must be between video.reconnect_delay_ms and 60000");
    check_range(video.stall_timeout_ms >= 1 && video.stall_timeout_ms <= 60000,
                "video.stall_timeout_ms", "must be between 1 and 60000");
    check_range(video.first_frame_timeout_ms >= 100 && video.first_frame_timeout_ms <= 120000,
                "video.first_frame_timeout_ms", "must be between 100 and 120000");
}

void validate(const Config& config) {
    validate_video(config.video);
    check_range(!config.detector.engine.empty(), "detector.engine", "must not be empty");
    check_range(std::isfinite(config.detector.confidence) &&
                    config.detector.confidence >= 0 && config.detector.confidence <= 1,
                "detector.confidence", "must be between 0 and 1");
    check_range(std::isfinite(config.detector.nms) && config.detector.nms >= 0 &&
                    config.detector.nms <= 1,
                "detector.nms", "must be between 0 and 1");
    check_range(!config.web.bind.empty(), "web.bind", "must not be empty");
    check_range(config.web.port >= 0 && config.web.port <= 65535,
                "web.port", "must be between 0 and 65535 (0 selects an ephemeral port)");
    check_range(!config.web.root.empty(), "web.root", "must not be empty");
    check_range(!config.recording.directory.empty(), "recording.directory", "must not be empty");
    check_range(config.recording.segment_seconds >= 1,
                "recording.segment_seconds", "must be positive");
    check_range(config.recording.max_storage_mb >= 1,
                "recording.max_storage_mb", "must be positive");
    check_range(config.recording.min_free_space_mb <= config.recording.max_storage_mb,
                "recording.min_free_space_mb", "must not exceed recording.max_storage_mb");
    check_range(!config.storage.database_path.empty(), "storage.database_path",
                "must not be empty");
    check_range(!config.storage.alert_directory.empty(), "storage.alert_directory",
                "must not be empty");
    for (std::size_t index = 0; index < config.alerts.size(); ++index) {
        const auto& rule = config.alerts[index];
        const auto field = "alerts[" + std::to_string(index) + "]";
        check_range(!rule.class_name.empty(), field + ".class", "must not be empty");
        check_range(std::isfinite(rule.confidence) && rule.confidence >= 0 &&
                        rule.confidence <= 1,
                    field + ".confidence", "must be between 0 and 1");
        check_range(rule.consecutive_frames >= 1,
                    field + ".consecutive_frames", "must be positive");
        check_range(rule.cooldown_seconds >= 0,
                    field + ".cooldown_seconds", "must not be negative");
        if (rule.roi) {
            const auto& roi = *rule.roi;
            check_range(std::isfinite(roi.x1) && std::isfinite(roi.y1) &&
                            std::isfinite(roi.x2) && std::isfinite(roi.y2) &&
                            roi.x1 >= 0 && roi.y1 >= 0 && roi.x2 <= 1 && roi.y2 <= 1,
                        field + ".roi", "coordinates must be between 0 and 1");
            check_range(roi.x1 < roi.x2 && roi.y1 < roi.y2,
                        field + ".roi", "must have x1 < x2 and y1 < y2");
        }
    }

    check_range(config.gps.source == "fixed", "gps.source", "must be fixed");
    check_range(std::isfinite(config.gps.latitude) && std::abs(config.gps.latitude) <= 90,
                "gps.latitude", "must be between -90 and 90");
    check_range(std::isfinite(config.gps.longitude) && std::abs(config.gps.longitude) <= 180,
                "gps.longitude", "must be between -180 and 180");
    check_range(std::isfinite(config.gps.altitude_m), "gps.altitude_m", "must be finite");
    check_range(config.webrtc.max_peers >= 1 && config.webrtc.max_peers <= 100,
                "webrtc.max_peers", "must be between 1 and 100");
    check_range(config.webrtc.connection_timeout_ms >= 1000 &&
                    config.webrtc.connection_timeout_ms <= 300000,
                "webrtc.connection_timeout_ms", "must be between 1000 and 300000");
    check_range(config.webrtc.media_queue_capacity >= 1 &&
                    config.webrtc.media_queue_capacity <= 120,
                "webrtc.media_queue_capacity", "must be between 1 and 120");
    check_range(config.webrtc.ice_log_verbosity >= 0 && config.webrtc.ice_log_verbosity <= 5,
                "webrtc.ice_log_verbosity", "must be between 0 and 5");
    check_range(!config.webrtc.enabled || !config.webrtc.host_interfaces.empty(),
                "webrtc.host_interfaces", "must not be empty when webrtc.enabled is true");
    for (const auto& interface : config.webrtc.host_interfaces) {
        check_range(!interface.empty() && interface.find_first_of(" /\t\r\n") == std::string::npos,
                    "webrtc.host_interfaces", "must contain interface names without spaces or slashes");
    }
    check_range(!config.whip.enabled || config.whip.url.rfind("https://", 0) == 0 ||
                    loopback_http_endpoint(config.whip.url),
                "whip.url", "must use https or http://127.0.0.1:<port>/<path> when enabled");
    check_range(!config.whip.enabled ||
                    (config.whip.url.size() > 8 &&
                     config.whip.url.find_first_of(" \t\r\n#") == std::string::npos),
                "whip.url", "must be a valid endpoint without whitespace or fragments");
}

Config parse(const YAML::Node& root) {
    check_keys(root, "", {"video", "detector", "web", "recording", "storage",
                           "alerts", "gps", "webrtc", "whip", "logging"});
    if (!root["video"] || !root["video"].IsMap() || !root["video"]["rtsp_url"]) {
        throw std::invalid_argument("video.rtsp_url is required");
    }
    Config config;
    const auto video = root["video"];
    check_keys(video, "video", {"rtsp_url", "username", "password", "transport", "latency_ms",
                                "reconnect_delay_ms", "max_reconnect_delay_ms",
                                "stall_timeout_ms", "first_frame_timeout_ms"});
    read_scalar(video, "rtsp_url", "video", config.video.rtsp_url);
    read_scalar(video, "username", "video", config.video.username);
    read_scalar(video, "password", "video", config.video.password);
    read_scalar(video, "transport", "video", config.video.transport);
    read_scalar(video, "latency_ms", "video", config.video.latency_ms);
    read_scalar(video, "reconnect_delay_ms", "video", config.video.reconnect_delay_ms);
    read_scalar(video, "max_reconnect_delay_ms", "video",
                config.video.max_reconnect_delay_ms);
    read_scalar(video, "stall_timeout_ms", "video", config.video.stall_timeout_ms);
    read_scalar(video, "first_frame_timeout_ms", "video", config.video.first_frame_timeout_ms);

    if (const auto section = root["detector"]) {
        check_keys(section, "detector", {"engine", "confidence", "nms", "annotate"});
        read_scalar(section, "engine", "detector", config.detector.engine);
        read_scalar(section, "confidence", "detector", config.detector.confidence);
        read_scalar(section, "nms", "detector", config.detector.nms);
        read_scalar(section, "annotate", "detector", config.detector.annotate);
    }
    if (const auto section = root["web"]) {
        check_keys(section, "web", {"bind", "port", "root"});
        read_scalar(section, "bind", "web", config.web.bind);
        read_scalar(section, "port", "web", config.web.port);
        read_scalar(section, "root", "web", config.web.root);
    }
    if (const auto section = root["recording"]) {
        check_keys(section, "recording", {"enabled", "directory", "segment_seconds",
                                            "max_storage_mb", "min_free_space_mb"});
        read_scalar(section, "enabled", "recording", config.recording.enabled);
        read_scalar(section, "directory", "recording", config.recording.directory);
        read_scalar(section, "segment_seconds", "recording", config.recording.segment_seconds);
        read_scalar(section, "max_storage_mb", "recording", config.recording.max_storage_mb);
        read_scalar(section, "min_free_space_mb", "recording", config.recording.min_free_space_mb);
    }
    if (const auto section = root["storage"]) {
        check_keys(section, "storage", {"database_path", "alert_directory"});
        read_scalar(section, "database_path", "storage", config.storage.database_path);
        read_scalar(section, "alert_directory", "storage", config.storage.alert_directory);
    }
    if (const auto section = root["alerts"]) {
        if (!section.IsSequence()) {
            throw std::invalid_argument("alerts must be a sequence");
        }
        for (std::size_t index = 0; index < section.size(); ++index) {
            const auto item = section[index];
            const auto path = "alerts[" + std::to_string(index) + "]";
            check_keys(item, path, {"class", "confidence", "consecutive_frames",
                                    "cooldown_seconds", "roi"});
            AlertRuleConfig rule;
            read_scalar(item, "class", path, rule.class_name);
            read_scalar(item, "confidence", path, rule.confidence);
            read_scalar(item, "consecutive_frames", path, rule.consecutive_frames);
            read_scalar(item, "cooldown_seconds", path, rule.cooldown_seconds);
            if (const auto roi = item["roi"]) {
                check_keys(roi, path + ".roi", {"x1", "y1", "x2", "y2"});
                for (const auto* coordinate : {"x1", "y1", "x2", "y2"}) {
                    if (!roi[coordinate]) {
                        throw std::invalid_argument(path + ".roi." + coordinate +
                                                    " is required");
                    }
                }
                AlertRoi value;
                read_scalar(roi, "x1", path + ".roi", value.x1);
                read_scalar(roi, "y1", path + ".roi", value.y1);
                read_scalar(roi, "x2", path + ".roi", value.x2);
                read_scalar(roi, "y2", path + ".roi", value.y2);
                rule.roi = value;
            }
            config.alerts.push_back(std::move(rule));
        }
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
        check_keys(section, "webrtc", {"enabled", "max_peers", "connection_timeout_ms",
                                       "media_queue_capacity", "host_interfaces",
                                       "ice_log_verbosity"});
        read_scalar(section, "enabled", "webrtc", config.webrtc.enabled);
        read_scalar(section, "max_peers", "webrtc", config.webrtc.max_peers);
        read_scalar(section, "connection_timeout_ms", "webrtc",
                    config.webrtc.connection_timeout_ms);
        read_scalar(section, "media_queue_capacity", "webrtc",
                    config.webrtc.media_queue_capacity);
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
    if (const auto section = root["whip"]) {
        check_keys(section, "whip", {"enabled", "url"});
        read_scalar(section, "enabled", "whip", config.whip.enabled);
        read_scalar(section, "url", "whip", config.whip.url);
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

bool validate_video_config(const VideoConfig& video, std::string& error) {
    try {
        validate_video(video);
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

} // namespace skai
