#pragma once

#include "skai/gps/gps_source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skai {

struct AlertDetection {
    int class_id = 0;
    std::string class_name;
    double confidence = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
};

struct AlertEvent {
    std::string id;
    std::int64_t timestamp_ms = 0;
    std::optional<GpsFix> gps;
    std::string snapshot_path;
    std::uint64_t frame_sequence = 0;
    std::optional<std::string> model_version;
    std::vector<AlertDetection> detections;
};

} // namespace skai
