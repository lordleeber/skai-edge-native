#pragma once

#include "skai/config.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace skai {

struct PublicConfigDto {
    std::string video_transport;
    int video_latency_ms = 0;
    int video_stall_timeout_ms = 0;
    double detector_confidence = 0.0;
    double detector_nms = 0.0;
    bool detector_annotate = false;
    std::string web_bind;
    int web_port = 0;
    bool recording_configured = false;
    int recording_segment_seconds = 0;
    bool gps_enabled = false;
    std::string gps_source;
    double gps_latitude = 0.0;
    double gps_longitude = 0.0;
    double gps_altitude_m = 0.0;
    bool webrtc_enabled = false;
    int webrtc_max_peers = 0;
};

struct DetectionDto {
    int class_id = 0;
    std::string class_name;
    float confidence = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

struct LatestDetectionsDto {
    bool available = false;
    std::uint64_t frame_sequence = 0;
    std::vector<DetectionDto> detections;
};

// Explicit API-facing DTO state. It intentionally omits credentials, RTSP URLs,
// engine paths, filesystem paths, and internal module objects.
class ApiState {
public:
    void configure(const Config& config) {
        PublicConfigDto dto;
        dto.video_transport = config.video.transport;
        dto.video_latency_ms = config.video.latency_ms;
        dto.video_stall_timeout_ms = config.video.stall_timeout_ms;
        dto.detector_confidence = config.detector.confidence;
        dto.detector_nms = config.detector.nms;
        dto.detector_annotate = config.detector.annotate;
        dto.web_bind = config.web.bind;
        dto.web_port = config.web.port;
        dto.recording_configured = config.recording.enabled;
        dto.recording_segment_seconds = config.recording.segment_seconds;
        dto.gps_enabled = config.gps.enabled;
        dto.gps_source = config.gps.source;
        dto.gps_latitude = config.gps.latitude;
        dto.gps_longitude = config.gps.longitude;
        dto.gps_altitude_m = config.gps.altitude_m;
        dto.webrtc_enabled = config.webrtc.enabled;
        dto.webrtc_max_peers = config.webrtc.max_peers;
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(dto);
        configured_ = true;
        detector_enabled_ = detector_supported_;
    }

    bool public_config(PublicConfigDto& output) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) return false;
        output = config_;
        return true;
    }

    void publish_detections(std::uint64_t frame_sequence,
                            std::vector<DetectionDto> detections) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.available = true;
        latest_.frame_sequence = frame_sequence;
        latest_.detections = std::move(detections);
    }

    LatestDetectionsDto latest_detections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    void set_detector_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        detector_enabled_ = enabled;
        if (!enabled) latest_ = {};
    }

    bool detector_enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return detector_enabled_;
    }

    void set_detector_supported(bool supported) {
        std::lock_guard<std::mutex> lock(mutex_);
        detector_supported_ = supported;
        if (!supported) detector_enabled_ = false;
    }

    bool detector_supported() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return detector_supported_;
    }

private:
    mutable std::mutex mutex_;
    bool configured_ = false;
    bool detector_enabled_ = true;
    bool detector_supported_ = true;
    PublicConfigDto config_;
    LatestDetectionsDto latest_;
};

} // namespace skai
