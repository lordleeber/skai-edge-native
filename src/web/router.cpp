#include "skai/web/router.hpp"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace skai::web {
namespace {

std::string json_string(const std::string& value) {
    std::string escaped = "\"";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(character));
        } else if (character == '\n') escaped += "\\n";
        else if (character == '\r') escaped += "\\r";
        else if (character == '\t') escaped += "\\t";
        else if (character >= 0x20) escaped.push_back(static_cast<char>(character));
    }
    escaped.push_back('"');
    return escaped;
}

const char* json_bool(bool value) { return value ? "true" : "false"; }

Response json_response(http::status result, unsigned version, std::string body) {
    Response response{result, version};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

std::string status_json(const StatusSnapshot& status, bool detector_enabled) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(6)
           << "{\"status\":\"" << status.status << "\",\"uptime_s\":"
           << status.uptime_s << ",\"video\":{\"fps\":";
    if (status.video_fps) output << *status.video_fps;
    else output << "null";
    output << "},\"detector\":{\"enabled\":" << json_bool(detector_enabled)
           << ",\"fps\":";
    if (status.detector_fps) output << *status.detector_fps;
    else output << "null";
    output << ",\"last_inference_ms\":";
    if (status.last_inference_ms) output << *status.last_inference_ms;
    else output << "null";
    output << "}}\n";
    return output.str();
}

std::string config_json(const PublicConfigDto& config, bool detector_enabled) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(6)
           << "{\"video\":{\"transport\":" << json_string(config.video_transport)
           << ",\"latency_ms\":" << config.video_latency_ms
           << ",\"stall_timeout_ms\":" << config.video_stall_timeout_ms
           << "},\"detector\":{\"enabled\":" << json_bool(detector_enabled)
           << ",\"confidence\":" << config.detector_confidence
           << ",\"nms\":" << config.detector_nms
           << ",\"annotate\":" << json_bool(config.detector_annotate)
           << "},\"web\":{\"bind\":" << json_string(config.web_bind)
           << ",\"port\":" << config.web_port
           << "},\"recording\":{\"configured\":"
           << json_bool(config.recording_configured)
           << ",\"segment_seconds\":" << config.recording_segment_seconds
           << "},\"gps\":{\"enabled\":" << json_bool(config.gps_enabled)
           << ",\"source\":" << json_string(config.gps_source)
           << "},\"webrtc\":{\"enabled\":" << json_bool(config.webrtc_enabled)
           << ",\"max_peers\":" << config.webrtc_max_peers << "}}\n";
    return output.str();
}

std::string detections_json(const LatestDetectionsDto& latest) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(6) << "{\"available\":"
           << json_bool(latest.available) << ",\"frame_sequence\":"
           << latest.frame_sequence << ",\"detections\":[";
    for (std::size_t index = 0; index < latest.detections.size(); ++index) {
        const auto& detection = latest.detections[index];
        if (index) output << ',';
        output << "{\"class_id\":" << detection.class_id
               << ",\"class_name\":" << json_string(detection.class_name)
               << ",\"confidence\":" << detection.confidence
               << ",\"box\":[" << detection.x1 << ',' << detection.y1 << ','
               << detection.x2 << ',' << detection.y2 << "]}";
    }
    output << "]}\n";
    return output.str();
}

bool alert_route(const std::string& path) {
    constexpr const char* prefix = "/api/v1/alerts/";
    return path == "/api/v1/alerts" ||
           (path.rfind(prefix, 0) == 0 && path.size() > std::char_traits<char>::length(prefix) &&
            path.find('/', std::char_traits<char>::length(prefix)) == std::string::npos);
}

} // namespace

Response route_request(const Request& request, const StatusSnapshot& status,
                       ApiState& api) {
    const std::string target(request.target());
    const auto query = target.find('?');
    const std::string path = target.substr(0, query);
    const bool get_route = path == "/health" || path == "/api/v1/status" ||
                           path == "/api/v1/config" ||
                           path == "/api/v1/detections/latest" ||
                           path == "/api/v1/gps" || alert_route(path) ||
                           path == "/api/v1/recordings";
    const bool post_route = path == "/api/v1/recording/start" ||
                            path == "/api/v1/recording/stop" ||
                            path == "/api/v1/detector/enable" ||
                            path == "/api/v1/detector/disable";
    if (!get_route && !post_route) {
        return json_response(http::status::not_found, request.version(),
                             "{\"error\":\"not found\"}\n");
    }
    const auto expected = get_route ? http::verb::get : http::verb::post;
    if (request.method() != expected) {
        auto response = json_response(http::status::method_not_allowed,
                                      request.version(),
                                      "{\"error\":\"method not allowed\"}\n");
        response.set(http::field::allow, get_route ? "GET" : "POST");
        return response;
    }
    if (path == "/health") {
        return json_response(http::status::ok, request.version(),
                             "{\"status\":\"ok\"}\n");
    }
    if (path == "/api/v1/status") {
        return json_response(http::status::ok, request.version(),
                             status_json(status, api.detector_enabled()));
    }
    if (path == "/api/v1/config") {
        PublicConfigDto config;
        if (!api.public_config(config)) {
            return json_response(http::status::service_unavailable, request.version(),
                                 "{\"available\":false}\n");
        }
        return json_response(http::status::ok, request.version(),
                             config_json(config, api.detector_enabled()));
    }
    if (path == "/api/v1/detections/latest") {
        return json_response(http::status::ok, request.version(),
                             detections_json(api.latest_detections()));
    }
    if (path == "/api/v1/gps") {
        const auto fix = api.latest_gps();
        if (!fix) {
            return json_response(http::status::service_unavailable, request.version(),
                                 "{\"available\":false}\n");
        }
        std::ostringstream body;
        body.imbue(std::locale::classic());
        body << std::setprecision(std::numeric_limits<double>::max_digits10)
             << "{\"available\":true,\"valid\":" << json_bool(fix->valid)
             << ",\"source\":" << json_string(fix->source)
             << ",\"latitude\":" << fix->latitude
             << ",\"longitude\":" << fix->longitude
             << ",\"altitude_m\":" << fix->altitude_m
             << ",\"hdop\":" << fix->hdop
             << ",\"satellites_visible\":" << fix->satellites_visible
             << ",\"satellites_used\":" << fix->satellites_used << "}\n";
        return json_response(http::status::ok, request.version(), body.str());
    }
    if (alert_route(path) || path == "/api/v1/recordings") {
        return json_response(http::status::service_unavailable, request.version(),
                             "{\"available\":false,\"items\":[]}\n");
    }
    if (path == "/api/v1/detector/enable" ||
        path == "/api/v1/detector/disable") {
        if (!api.detector_supported()) {
            return json_response(http::status::service_unavailable, request.version(),
                                 "{\"available\":false}\n");
        }
        const bool enabled = path == "/api/v1/detector/enable";
        api.set_detector_enabled(enabled);
        return json_response(http::status::ok, request.version(),
                             std::string("{\"enabled\":") + json_bool(enabled) + "}\n");
    }
    return json_response(http::status::not_implemented, request.version(),
                         "{\"available\":false,\"error\":\"recording not implemented\"}\n");
}

} // namespace skai::web
