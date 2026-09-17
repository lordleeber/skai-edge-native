#pragma once

#include "skai/alerts/alert_event.hpp"
#include "skai/config.hpp"
#include "skai/events.hpp"
#include "skai/gps/gps_state.hpp"
#include "skai/inference/yolo_postprocess.hpp"
#include "skai/storage/alert_repository.hpp"
#include "skai/video/frame.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skai {

class AlertManager {
public:
    AlertManager(std::shared_ptr<GpsState> gps = {},
                 std::shared_ptr<EventChannel> events = {},
                 std::shared_ptr<AlertRepository> repository = {},
                 Logger* logger = nullptr);

    void configure(std::vector<AlertRuleConfig> rules,
                   std::string snapshot_directory = {},
                   std::string model_version = {});
    std::vector<AlertEvent> process(
        const DetectionResult& result, int frame_width, int frame_height,
        const std::vector<std::string>& class_names,
        std::uint64_t detector_generation = 0,
        std::chrono::system_clock::time_point wall_time =
            std::chrono::system_clock::now(),
        std::chrono::steady_clock::time_point monotonic_time =
            std::chrono::steady_clock::now()) {
        return process(result, frame_width, frame_height, class_names,
                       detector_generation, nullptr, wall_time, monotonic_time);
    }
    std::vector<AlertEvent> process(
        const DetectionResult& result, int frame_width, int frame_height,
        const std::vector<std::string>& class_names,
        std::uint64_t detector_generation, const Frame* snapshot_frame,
        std::chrono::system_clock::time_point wall_time =
            std::chrono::system_clock::now(),
        std::chrono::steady_clock::time_point monotonic_time =
            std::chrono::steady_clock::now());
    bool cleanup_oldest(std::size_t keep, std::string& error);

private:
    struct RuleState {
        AlertRuleConfig rule;
        int consecutive = 0;
        std::optional<std::chrono::steady_clock::time_point> last_alert;
    };

    bool persist(const Frame& frame, AlertEvent& alert, std::string& error);
    bool reconcile_deletions(std::string& error);
    void report_error(const std::string& error) const;

    std::shared_ptr<GpsState> gps_;
    std::shared_ptr<EventChannel> events_;
    std::shared_ptr<AlertRepository> repository_;
    Logger* logger_ = nullptr;
    std::string snapshot_directory_;
    std::string model_version_;
    std::vector<RuleState> rules_;
    std::optional<std::uint64_t> detector_generation_;
    std::uint64_t next_id_ = 1;
};

} // namespace skai
