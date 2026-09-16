#pragma once

#include "skai/inference/preprocess.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skai {

struct Detection {
    int class_id = 0;
    float confidence = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

struct DetectionResult {
    std::uint64_t frame_sequence = 0;
    std::vector<Detection> detections;
};

struct YoloPostprocessConfig {
    float confidence_threshold = 0.25f;
    float nms_iou_threshold = 0.45f;
    std::size_t max_detections = 300;
};

// Raw YOLO11 detection output is channel-major [1, 4 + classes, candidates]:
// center-x, center-y, width, height, followed by one score per class.
struct YoloOutputView {
    const float* data = nullptr;
    std::size_t elements = 0;
    int channels = 0;
    int candidates = 0;
};

bool postprocess_yolo(const YoloOutputView& output, const PreprocessPlan& plan,
                      const YoloPostprocessConfig& config,
                      std::uint64_t frame_sequence, DetectionResult& result,
                      std::string& error);

} // namespace skai
