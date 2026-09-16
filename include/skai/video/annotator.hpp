#pragma once

#include "skai/inference/yolo_postprocess.hpp"
#include "skai/video/frame.hpp"

#include <string>
#include <vector>

namespace skai {

struct AnnotationOptions {
    bool enabled = true;
    bool show_metrics = false;
    double fps = 0.0;
    double inference_ms = 0.0;
    int box_thickness = 2;
    double font_scale = 0.5;
};

// Returns a readable class label, falling back to class_<id> when names are absent.
std::string detection_label(const Detection& detection,
                            const std::vector<std::string>& class_names);

// Copies the source frame before drawing. The source and its detections are immutable;
// output must be a distinct Frame object and the sequence numbers must match.
bool annotate_frame(const Frame& source, const DetectionResult& detections,
                    const std::vector<std::string>& class_names,
                    const AnnotationOptions& options, Frame& output,
                    std::string& error);

} // namespace skai
