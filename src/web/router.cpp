#include "skai/web/router.hpp"

#include <charconv>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>

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

} // namespace

std::string webrtc_diagnostics_json(const WebRtcDiagnostics& diagnostics) {
    std::ostringstream output;
    const auto write_peer = [&output](const WebRtcPeerDiagnostics& peer) {
        output << "{\"session_id\":" << json_string(peer.session_id)
               << ",\"peer_state\":" << json_string(peer.peer_state)
               << ",\"ice_state\":" << json_string(peer.ice_state)
               << ",\"local_candidate\":" << json_string(peer.local_candidate)
               << ",\"local_interface\":" << json_string(peer.local_interface)
               << ",\"selected_interface\":" << json_string(peer.selected_interface)
               << ",\"connection_age_s\":" << peer.connection_age_s
               << ",\"bytes_sent\":" << peer.bytes_sent
               << ",\"packets_sent\":" << peer.packets_sent
               << ",\"packets_retransmitted\":" << peer.packets_retransmitted
               << ",\"media_queue_drops\":" << peer.media_queue_drops
               << ",\"keyframe_events\":" << peer.keyframe_events
               << ",\"failure_stage\":" << json_string(peer.failure_stage)
               << ",\"close_reason\":" << json_string(peer.close_reason)
               << ",\"last_error\":" << json_string(peer.last_error) << '}';
    };
    output << "{\"enabled\":" << json_bool(diagnostics.enabled)
           << ",\"lan_only\":" << json_bool(diagnostics.lan_only)
           << ",\"max_peers\":" << diagnostics.max_peers
           << ",\"active_peers\":" << diagnostics.peers.size()
           << ",\"sessions_created\":" << diagnostics.sessions_created
           << ",\"sessions_closed\":" << diagnostics.sessions_closed
           << ",\"signaling_errors\":" << diagnostics.signaling_errors
           << ",\"media_errors\":" << diagnostics.media_errors
           << ",\"last_close_reason\":"
           << json_string(diagnostics.last_close_reason) << ",\"peers\":[";
    for (std::size_t index = 0; index < diagnostics.peers.size(); ++index) {
        if (index) output << ',';
        write_peer(diagnostics.peers[index]);
    }
    output << "],\"recently_closed\":[";
    for (std::size_t index = 0; index < diagnostics.recently_closed.size(); ++index) {
        if (index) output << ',';
        write_peer(diagnostics.recently_closed[index]);
    }
    output << "]}";
    return output.str();
}

namespace {

std::string status_json(const StatusSnapshot& status, bool detector_enabled,
                        WebRtcManager* webrtc) {
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
    output << "},\"encoder\":";
    if (!status.encoder) {
        output << "null";
    } else {
        const auto& encoder = *status.encoder;
        output << "{\"frames_submitted\":" << encoder.frames_submitted
               << ",\"frames_rejected\":" << encoder.frames_rejected
               << ",\"frames_dropped\":" << encoder.frames_dropped
               << ",\"appsrc_pressure_dropped\":"
               << encoder.appsrc_pressure_dropped
               << ",\"access_units_encoded\":" << encoder.access_units_encoded
               << ",\"bytes_encoded\":" << encoder.bytes_encoded
               << ",\"access_units_dropped\":" << encoder.access_units_dropped
               << ",\"last_access_unit_age_ms\":"
               << encoder.last_access_unit_age_ms
               << ",\"last_error\":" << json_string(encoder.last_error) << '}';
    }
    output << ",\"webrtc\":";
    if (webrtc) output << webrtc_diagnostics_json(webrtc->diagnostics());
    else output << "null";
    output << "}\n";
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
           << ",\"max_peers\":" << config.webrtc_max_peers
           << ",\"connection_timeout_ms\":"
           << config.webrtc_connection_timeout_ms
           << ",\"media_queue_capacity\":"
           << config.webrtc_media_queue_capacity << "}}\n";
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

std::string alert_json(const AlertEvent& alert) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"id\":" << json_string(alert.id)
           << ",\"timestamp_ms\":" << alert.timestamp_ms << ",\"gps\":";
    if (alert.gps) {
        output << "{\"valid\":" << json_bool(alert.gps->valid)
               << ",\"source\":" << json_string(alert.gps->source)
               << ",\"latitude\":" << alert.gps->latitude
               << ",\"longitude\":" << alert.gps->longitude
               << ",\"altitude_m\":" << alert.gps->altitude_m << '}';
    } else output << "null";
    output << ",\"snapshot_path\":" << json_string(alert.snapshot_path)
           << ",\"frame_sequence\":" << alert.frame_sequence
           << ",\"model_version\":";
    if (alert.model_version) output << json_string(*alert.model_version);
    else output << "null";
    output << ",\"detections\":[";
    for (std::size_t i = 0; i < alert.detections.size(); ++i) {
        const auto& detection = alert.detections[i];
        if (i) output << ',';
        output << "{\"class_id\":" << detection.class_id
               << ",\"class_name\":" << json_string(detection.class_name)
               << ",\"confidence\":" << detection.confidence
               << ",\"box\":[" << detection.x1 << ',' << detection.y1 << ','
               << detection.x2 << ',' << detection.y2 << "]}";
    }
    output << "]}";
    return output.str();
}

std::string alerts_json(const std::vector<AlertEvent>& alerts) {
    std::string result = "{\"available\":true,\"items\":[";
    for (std::size_t i = 0; i < alerts.size(); ++i) {
        if (i) result += ',';
        result += alert_json(alerts[i]);
    }
    return result + "]}\n";
}

bool decode_component(const std::string& encoded, std::string& decoded) {
    decoded.clear();
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') { decoded.push_back(encoded[i]); continue; }
        if (i + 2 >= encoded.size()) return false;
        unsigned value = 0;
        const auto parsed = std::from_chars(encoded.data() + i + 1,
                                            encoded.data() + i + 3, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + i + 3) return false;
        decoded.push_back(static_cast<char>(value));
        i += 2;
    }
    return true;
}

bool query_parameters(const std::string& target,
                      std::unordered_map<std::string, std::string>& values) {
    auto position = target.find('?');
    while (position != std::string::npos && position + 1 < target.size()) {
        const auto end = target.find('&', position + 1);
        const auto equal = target.find('=', position + 1);
        if (equal == std::string::npos || (end != std::string::npos && equal > end)) break;
        std::string value;
        if (!decode_component(target.substr(equal + 1, end - equal - 1), value)) return false;
        values[target.substr(position + 1, equal - position - 1)] = std::move(value);
        position = end;
    }
    return true;
}

template <typename Number>
bool parse_number(const std::string& text, Number& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool alert_route(const std::string& path) {
    constexpr const char* prefix = "/api/v1/alerts/";
    return path == "/api/v1/alerts" ||
           (path.rfind(prefix, 0) == 0 && path.size() > std::char_traits<char>::length(prefix) &&
            path.find('/', std::char_traits<char>::length(prefix)) == std::string::npos);
}

} // namespace

Response route_request(const Request& request, const StatusSnapshot& status,
                       ApiState& api, AlertRepository* alerts,
                       RecordingController* recording, WebRtcManager* webrtc) {
    const std::string target(request.target());
    const auto query = target.find('?');
    const std::string path = target.substr(0, query);
    const bool get_route = path == "/health" || path == "/api/v1/status" ||
                           path == "/api/v1/metrics" ||
                           path == "/api/v1/config" ||
                           path == "/api/v1/detections/latest" ||
                           path == "/api/v1/gps" || alert_route(path) ||
                           path == "/api/v1/recordings";
    const bool post_route = path == "/api/v1/recording/start" ||
                            path == "/api/v1/recording/stop" ||
                            path == "/api/v1/detector/enable" ||
                            path == "/api/v1/detector/disable" ||
                            path == "/api/v1/webrtc/whep";
    constexpr std::string_view session_prefix = "/api/v1/webrtc/sessions/";
    const bool delete_route = path.rfind(session_prefix, 0) == 0 &&
        path.size() > session_prefix.size() &&
        path.find('/', session_prefix.size()) == std::string::npos;
    if (!get_route && !post_route && !delete_route) {
        return json_response(http::status::not_found, request.version(),
                             "{\"error\":\"not found\"}\n");
    }
    const auto expected = get_route ? http::verb::get :
                          delete_route ? http::verb::delete_ : http::verb::post;
    if (request.method() != expected) {
        auto response = json_response(http::status::method_not_allowed,
                                      request.version(),
                                      "{\"error\":\"method not allowed\"}\n");
        response.set(http::field::allow, get_route ? "GET" :
                     delete_route ? "DELETE" : "POST");
        return response;
    }
    if (path == "/health") {
        return json_response(http::status::ok, request.version(),
                             "{\"status\":\"ok\"}\n");
    }
    if (path == "/api/v1/status") {
        return json_response(http::status::ok, request.version(),
                             status_json(status, api.detector_enabled(), webrtc));
    }
    if (path == "/api/v1/metrics") {
        const auto diagnostics = webrtc ? webrtc->diagnostics() : WebRtcDiagnostics{};
        return json_response(http::status::ok, request.version(),
                             std::string("{\"webrtc\":") +
                                 webrtc_diagnostics_json(diagnostics) + "}\n");
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
    if (path == "/api/v1/webrtc/whep") {
        if (!webrtc) {
            return json_response(http::status::service_unavailable, request.version(),
                                 "{\"error\":\"WebRTC unavailable\"}\n");
        }
        if (request[http::field::content_type] != "application/sdp") {
            return json_response(http::status::unsupported_media_type, request.version(),
                                 "{\"error\":\"Content-Type must be application/sdp\"}\n");
        }
        const auto created = webrtc->create_session(request.body());
        if (!created) {
            http::status result = http::status::internal_server_error;
            if (created.error == CreateSessionError::InvalidOffer) {
                result = http::status::bad_request;
            } else if (created.error == CreateSessionError::Disabled) {
                result = http::status::service_unavailable;
            } else if (created.error == CreateSessionError::Capacity) {
                result = http::status::too_many_requests;
            } else if (created.error == CreateSessionError::GatheringTimeout) {
                result = http::status::gateway_timeout;
            }
            return json_response(result, request.version(),
                                 "{\"error\":" + json_string(created.message) + "}\n");
        }
        Response response{http::status::created, request.version()};
        response.set(http::field::content_type, "application/sdp");
        response.set(http::field::cache_control, "no-store");
        response.set(http::field::location,
                     std::string(session_prefix) + created.session_id);
        response.body() = created.answer_sdp;
        response.prepare_payload();
        return response;
    }
    if (delete_route) {
        if (!webrtc) {
            return json_response(http::status::service_unavailable, request.version(),
                                 "{\"error\":\"WebRTC unavailable\"}\n");
        }
        if (!webrtc->close_session(path.substr(session_prefix.size()))) {
            return json_response(http::status::not_found, request.version(),
                                 "{\"error\":\"session not found\"}\n");
        }
        Response response{http::status::no_content, request.version()};
        response.prepare_payload();
        return response;
    }
    if (alert_route(path)) {
        if (!alerts) {
            return json_response(http::status::service_unavailable, request.version(),
                                 "{\"available\":false,\"items\":[]}\n");
        }
        std::string error;
        if (path != "/api/v1/alerts") {
            const auto alert = alerts->find_by_id(path.substr(15), error);
            if (!error.empty()) return json_response(http::status::internal_server_error,
                request.version(), "{\"error\":" + json_string(error) + "}\n");
            if (!alert) return json_response(http::status::not_found, request.version(),
                                             "{\"error\":\"alert not found\"}\n");
            return json_response(http::status::ok, request.version(), alert_json(*alert) + "\n");
        }
        std::unordered_map<std::string, std::string> parameters;
        if (!query_parameters(target, parameters)) {
            return json_response(http::status::bad_request, request.version(),
                                 "{\"error\":\"invalid query encoding\"}\n");
        }
        std::size_t limit = 100;
        if (const auto it = parameters.find("limit"); it != parameters.end()) {
            if (!parse_number(it->second, limit) || limit == 0 || limit > 1000) {
                return json_response(http::status::bad_request, request.version(),
                                     "{\"error\":\"invalid limit\"}\n");
            }
        }
        std::vector<AlertEvent> items;
        const auto from = parameters.find("from");
        const auto to = parameters.find("to");
        const auto class_name = parameters.find("class");
        if ((from == parameters.end()) != (to == parameters.end()) ||
            (class_name != parameters.end() && from != parameters.end())) {
            return json_response(http::status::bad_request, request.version(),
                                 "{\"error\":\"invalid alert filters\"}\n");
        }
        if (from != parameters.end()) {
            std::int64_t from_ms = 0, to_ms = 0;
            if (!parse_number(from->second, from_ms) || !parse_number(to->second, to_ms)) {
                return json_response(http::status::bad_request, request.version(),
                                     "{\"error\":\"invalid time range\"}\n");
            }
            items = alerts->find_by_time_range(from_ms, to_ms, limit, error);
        } else if (class_name != parameters.end()) {
            items = alerts->find_by_class(class_name->second, limit, error);
        } else items = alerts->find_recent(limit, error);
        if (!error.empty()) return json_response(http::status::internal_server_error,
            request.version(), "{\"error\":" + json_string(error) + "}\n");
        return json_response(http::status::ok, request.version(), alerts_json(items));
    }
    if (path == "/api/v1/recordings") {
        if (recording && recording->status().configured) {
            const auto recording_status = recording->status();
            return json_response(http::status::ok, request.version(),
                std::string("{\"available\":true,\"state\":") +
                json_string(recording_status.state) + ",\"active\":" +
                json_bool(recording_status.active) + ",\"current_path\":" +
                json_string(recording_status.current_path) + ",\"access_units_written\":" +
                std::to_string(recording_status.access_units_written) + "}\n");
        }
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
    if (recording && recording->status().configured) {
        std::string error;
        const bool started = path == "/api/v1/recording/start";
        if ((started ? recording->start(error) : recording->stop(error))) {
            const auto recording_status = recording->status();
            return json_response(http::status::ok, request.version(),
                std::string("{\"state\":") + json_string(recording_status.state) +
                ",\"active\":" + json_bool(recording_status.active) +
                ",\"current_path\":" + json_string(recording_status.current_path) +
                ",\"last_error\":" + json_string(recording_status.last_error) + "}\n");
        }
        return json_response(http::status::service_unavailable, request.version(),
                             "{\"error\":" + json_string(error) + "}\n");
    }
    return json_response(http::status::not_implemented, request.version(),
                         "{\"available\":false,\"error\":\"recording not implemented\"}\n");
}

} // namespace skai::web
