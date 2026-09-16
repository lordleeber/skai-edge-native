#include "skai/inference/yolo_postprocess.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skai {
namespace {

struct Candidate {
    Detection detection;
    std::size_t index = 0;
};

bool valid_plan(const PreprocessPlan& plan) {
    return plan.source_width > 0 && plan.source_height > 0 &&
           plan.width > 0 && plan.height > 0 &&
           plan.resized_width > 0 && plan.resized_height > 0 &&
           plan.pad_left >= 0 && plan.pad_top >= 0 &&
           static_cast<std::int64_t>(plan.pad_left) + plan.resized_width <= plan.width &&
           static_cast<std::int64_t>(plan.pad_top) + plan.resized_height <= plan.height;
}

double intersection_over_union(const Detection& a, const Detection& b) {
    const double left = std::max(a.x1, b.x1);
    const double top = std::max(a.y1, b.y1);
    const double right = std::min(a.x2, b.x2);
    const double bottom = std::min(a.y2, b.y2);
    const double intersection = std::max(0.0, right - left) *
                                std::max(0.0, bottom - top);
    const double area_a = static_cast<double>(a.x2 - a.x1) * (a.y2 - a.y1);
    const double area_b = static_cast<double>(b.x2 - b.x1) * (b.y2 - b.y1);
    const double total = area_a + area_b - intersection;
    return total > 0.0 ? intersection / total : 0.0;
}

} // namespace

bool postprocess_yolo(const YoloOutputView& output, const PreprocessPlan& plan,
                      const YoloPostprocessConfig& config,
                      std::uint64_t frame_sequence, DetectionResult& result,
                      std::string& error) {
    result = {};
    error.clear();
    if (!output.data || output.channels <= 4 || output.candidates <= 0 ||
        static_cast<std::size_t>(output.channels) >
            std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(output.candidates) ||
        output.elements != static_cast<std::size_t>(output.channels) *
                               output.candidates) {
        error = "YOLO output shape or buffer is invalid";
        return false;
    }
    if (!valid_plan(plan)) {
        error = "YOLO preprocessing plan is invalid";
        return false;
    }
    if (!std::isfinite(config.confidence_threshold) ||
        !std::isfinite(config.nms_iou_threshold) ||
        config.confidence_threshold < 0.0f || config.confidence_threshold > 1.0f ||
        config.nms_iou_threshold < 0.0f || config.nms_iou_threshold > 1.0f ||
        config.max_detections == 0) {
        error = "YOLO confidence/NMS threshold or max detections is invalid";
        return false;
    }

    const auto count = static_cast<std::size_t>(output.candidates);
    const double x_scale = static_cast<double>(plan.source_width) / plan.resized_width;
    const double y_scale = static_cast<double>(plan.source_height) / plan.resized_height;
    std::vector<Candidate> filtered;
    filtered.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        float score = -std::numeric_limits<float>::infinity();
        int class_id = -1;
        for (int channel = 4; channel < output.channels; ++channel) {
            const float value = output.data[static_cast<std::size_t>(channel) * count +
                                            index];
            if (std::isfinite(value) && value > score) {
                score = value;
                class_id = channel - 4;
            }
        }
        if (class_id < 0 || score <= config.confidence_threshold) continue;

        const float cx = output.data[index];
        const float cy = output.data[count + index];
        const float width = output.data[2 * count + index];
        const float height = output.data[3 * count + index];
        if (!std::isfinite(cx) || !std::isfinite(cy) ||
            !std::isfinite(width) || !std::isfinite(height) ||
            width <= 0.0f || height <= 0.0f) continue;

        const double x1 = (static_cast<double>(cx) - width / 2.0 -
                           plan.pad_left) * x_scale;
        const double x2 = (static_cast<double>(cx) + width / 2.0 -
                           plan.pad_left) * x_scale;
        const double y1 = (static_cast<double>(cy) - height / 2.0 -
                           plan.pad_top) * y_scale;
        const double y2 = (static_cast<double>(cy) + height / 2.0 -
                           plan.pad_top) * y_scale;
        if (!std::isfinite(x1) || !std::isfinite(x2) ||
            !std::isfinite(y1) || !std::isfinite(y2)) continue;

        Detection detection;
        detection.class_id = class_id;
        detection.confidence = score;
        detection.x1 = static_cast<float>(std::clamp(x1, 0.0,
                                                     static_cast<double>(plan.source_width)));
        detection.y1 = static_cast<float>(std::clamp(y1, 0.0,
                                                     static_cast<double>(plan.source_height)));
        detection.x2 = static_cast<float>(std::clamp(x2, 0.0,
                                                     static_cast<double>(plan.source_width)));
        detection.y2 = static_cast<float>(std::clamp(y2, 0.0,
                                                     static_cast<double>(plan.source_height)));
        if (detection.x2 <= detection.x1 || detection.y2 <= detection.y1) continue;
        filtered.push_back({detection, index});
    }

    std::sort(filtered.begin(), filtered.end(), [](const Candidate& a,
                                                     const Candidate& b) {
        if (a.detection.confidence != b.detection.confidence) {
            return a.detection.confidence > b.detection.confidence;
        }
        return a.index < b.index;
    });

    result.frame_sequence = frame_sequence;
    result.detections.reserve(std::min(config.max_detections, filtered.size()));
    for (const auto& candidate : filtered) {
        bool suppressed = false;
        for (const auto& kept : result.detections) {
            if (candidate.detection.class_id == kept.class_id &&
                intersection_over_union(candidate.detection, kept) >
                    config.nms_iou_threshold) {
                suppressed = true;
                break;
            }
        }
        if (suppressed) continue;
        result.detections.push_back(candidate.detection);
        if (result.detections.size() == config.max_detections) break;
    }
    return true;
}

} // namespace skai
