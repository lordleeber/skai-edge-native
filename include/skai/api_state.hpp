#pragma once

#include "skai/config.hpp"
#include "skai/gps/gps_state.hpp"
#include "skai/status.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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
    bool webrtc_enabled = false;
    int webrtc_max_peers = 0;
    int webrtc_connection_timeout_ms = 0;
    int webrtc_media_queue_capacity = 0;
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
    std::uint64_t pts_ns = 0;
    int frame_width = 0;
    int frame_height = 0;
    std::vector<DetectionDto> detections;
};

struct DetectorPermit {
    bool enabled = false;
    std::uint64_t generation = 0;
};

// Explicit API-facing DTO state. It intentionally omits credentials, RTSP URLs,
// engine paths, filesystem paths, and internal module objects.
class ApiState {
public:
    explicit ApiState(std::shared_ptr<RuntimeStatus> status = {},
                      std::shared_ptr<GpsState> gps_state = {})
        : status_(std::move(status)),
          gps_state_(gps_state ? std::move(gps_state)
                               : std::make_shared<GpsState>()) {}

    void bind_runtime_status(std::shared_ptr<RuntimeStatus> status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::move(status);
        synchronize_runtime_status();
    }

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
        dto.webrtc_enabled = config.webrtc.enabled;
        dto.webrtc_max_peers = config.webrtc.max_peers;
        dto.webrtc_connection_timeout_ms = config.webrtc.connection_timeout_ms;
        dto.webrtc_media_queue_capacity = config.webrtc.media_queue_capacity;
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(dto);
        configured_ = true;
        detector_enabled_ = detector_supported_;
        ++detector_generation_;
        latest_ = {};
        synchronize_runtime_status();
    }

    bool public_config(PublicConfigDto& output) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) return false;
        output = config_;
        return true;
    }

    DetectorPermit detector_permit() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {detector_enabled_, detector_generation_};
    }

    template <typename Commit>
    bool commit_detections(const DetectorPermit& permit,
                           std::uint64_t frame_sequence,
                           std::vector<DetectionDto> detections,
                           Commit&& commit) {
        return commit_detections(permit, frame_sequence, 0, 0, 0,
                                 std::move(detections),
                                 std::forward<Commit>(commit));
    }

    template <typename Commit>
    bool commit_detections(const DetectorPermit& permit,
                           std::uint64_t frame_sequence,
                           int frame_width, int frame_height,
                           std::uint64_t pts_ns,
                           std::vector<DetectionDto> detections,
                           Commit&& commit) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!detector_enabled_ || permit.generation != detector_generation_) {
            return false;
        }
        latest_.available = true;
        latest_.frame_sequence = frame_sequence;
        latest_.frame_width = frame_width;
        latest_.frame_height = frame_height;
        latest_.pts_ns = pts_ns;
        latest_.detections = std::move(detections);
        std::forward<Commit>(commit)();
        return true;
    }

    template <typename Commit>
    bool invalidate_detections(const DetectorPermit& permit, Commit&& commit) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!detector_enabled_ || permit.generation != detector_generation_) {
            return false;
        }
        const bool was_available = latest_.available;
        latest_ = {};
        std::forward<Commit>(commit)(was_available);
        return true;
    }

    LatestDetectionsDto latest_detections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    std::optional<GpsFix> latest_gps() const {
        return gps_state_->latest();
    }

    void set_detector_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        detector_enabled_ = enabled && detector_supported_;
        ++detector_generation_;
        latest_ = {};
        synchronize_runtime_status();
    }

    bool detector_enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return detector_enabled_;
    }

    void set_detector_supported(bool supported) {
        std::lock_guard<std::mutex> lock(mutex_);
        detector_supported_ = supported;
        if (!supported) {
            detector_enabled_ = false;
            ++detector_generation_;
            latest_ = {};
        }
        synchronize_runtime_status();
    }

    bool detector_supported() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return detector_supported_;
    }

private:
    void synchronize_runtime_status() {
        if (!status_) return;
        status_->clear_detector();
        status_->set_detector_expected(detector_enabled_);
    }

    mutable std::mutex mutex_;
    bool configured_ = false;
    bool detector_enabled_ = true;
    bool detector_supported_ = true;
    std::uint64_t detector_generation_ = 0;
    std::shared_ptr<RuntimeStatus> status_;
    std::shared_ptr<GpsState> gps_state_;
    PublicConfigDto config_;
    LatestDetectionsDto latest_;
};

} // namespace skai
