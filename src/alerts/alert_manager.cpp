#include "skai/alerts/alert_manager.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace skai {
namespace {

std::string json_string(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(character));
        } else if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else if (character == '\t') result += "\\t";
        else if (character >= 0x20) result.push_back(static_cast<char>(character));
    }
    result.push_back('"');
    return result;
}

std::string class_name(int class_id, const std::vector<std::string>& names) {
    return class_id >= 0 && static_cast<std::size_t>(class_id) < names.size()
               ? names[class_id] : "class_" + std::to_string(class_id);
}

bool in_roi(const Detection& detection, int width, int height,
            const std::optional<AlertRoi>& roi) {
    if (!roi) return true;
    if (width <= 0 || height <= 0) return false;
    const double center_x = (detection.x1 + detection.x2) * 0.5 / width;
    const double center_y = (detection.y1 + detection.y2) * 0.5 / height;
    return center_x >= roi->x1 && center_x <= roi->x2 &&
           center_y >= roi->y1 && center_y <= roi->y2;
}

std::string event_data(const AlertEvent& event) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"id\":" << json_string(event.id)
           << ",\"frame_sequence\":" << event.frame_sequence
           << ",\"timestamp_ms\":" << event.timestamp_ms
           << ",\"detections\":[";
    for (std::size_t index = 0; index < event.detections.size(); ++index) {
        const auto& detection = event.detections[index];
        if (index) output << ',';
        output << "{\"class_id\":" << detection.class_id
               << ",\"class_name\":" << json_string(detection.class_name)
               << ",\"confidence\":" << detection.confidence
               << ",\"box\":[" << detection.x1 << ',' << detection.y1 << ','
               << detection.x2 << ',' << detection.y2 << "]}";
    }
    output << ']';
    if (event.gps && event.gps->valid) {
        output << ",\"gps_valid\":true,\"gps_source\":"
               << json_string(event.gps->source)
               << ",\"latitude\":" << event.gps->latitude
               << ",\"longitude\":" << event.gps->longitude
               << ",\"altitude_m\":" << event.gps->altitude_m;
    } else {
        output << ",\"gps_valid\":false,\"gps_source\":\"none\"";
    }
    output << '}';
    return output.str();
}

} // namespace

AlertManager::AlertManager(std::shared_ptr<GpsState> gps,
                           std::shared_ptr<EventChannel> events)
    : gps_(std::move(gps)), events_(std::move(events)) {}

void AlertManager::configure(std::vector<AlertRuleConfig> rules) {
    rules_.clear();
    rules_.reserve(rules.size());
    for (auto& rule : rules) rules_.push_back({std::move(rule), 0, std::nullopt});
}

std::vector<AlertEvent> AlertManager::process(
    const DetectionResult& result, int frame_width, int frame_height,
    const std::vector<std::string>& class_names,
    std::chrono::system_clock::time_point wall_time,
    std::chrono::steady_clock::time_point monotonic_time) {
    std::vector<AlertEvent> alerts;
    for (std::size_t rule_index = 0; rule_index < rules_.size(); ++rule_index) {
        auto& state = rules_[rule_index];
        std::vector<AlertDetection> matching;
        for (const auto& detection : result.detections) {
            const auto name = class_name(detection.class_id, class_names);
            if (!std::isfinite(detection.confidence) ||
                !std::isfinite(detection.x1) || !std::isfinite(detection.y1) ||
                !std::isfinite(detection.x2) || !std::isfinite(detection.y2) ||
                name != state.rule.class_name ||
                detection.confidence < state.rule.confidence ||
                !in_roi(detection, frame_width, frame_height, state.rule.roi)) continue;
            matching.push_back({detection.class_id, name, detection.confidence,
                                detection.x1, detection.y1, detection.x2, detection.y2});
        }
        if (matching.empty()) {
            state.consecutive = 0;
            continue;
        }
        state.consecutive = std::min(state.consecutive + 1,
                                     state.rule.consecutive_frames);
        if (state.consecutive < state.rule.consecutive_frames) continue;
        if (state.last_alert &&
            monotonic_time - *state.last_alert <
                std::chrono::seconds(state.rule.cooldown_seconds)) continue;

        AlertEvent alert;
        alert.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            wall_time.time_since_epoch()).count();
        alert.id = "alert-" + std::to_string(alert.timestamp_ms) + '-' +
                   std::to_string(result.frame_sequence) + '-' +
                   std::to_string(rule_index) + '-' + std::to_string(next_id_++);
        alert.frame_sequence = result.frame_sequence;
        alert.gps = gps_ ? gps_->latest() : std::nullopt;
        alert.detections = std::move(matching);
        state.consecutive = 0;
        state.last_alert = monotonic_time;
        if (events_) events_->publish(EventType::Alert, event_data(alert));
        alerts.push_back(std::move(alert));
    }
    return alerts;
}

} // namespace skai
