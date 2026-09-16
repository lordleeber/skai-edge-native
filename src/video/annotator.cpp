#include "skai/video/annotator.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace skai {
namespace {

bool valid_frame(const Frame& frame, std::string& error) {
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < 0 ||
        frame.width > std::numeric_limits<int>::max() / 3) {
        error = "annotation frame dimensions or stride are invalid";
        return false;
    }
    const auto row_bytes = static_cast<std::size_t>(frame.width) * 3;
    const auto stride = static_cast<std::size_t>(frame.stride);
    if (stride < row_bytes) {
        error = "annotation frame stride is shorter than a BGR row";
        return false;
    }
    const auto rows_before_last = static_cast<std::size_t>(frame.height - 1);
    if (rows_before_last >
            (std::numeric_limits<std::size_t>::max() - row_bytes) / stride ||
        frame.bgr.size() < rows_before_last * stride + row_bytes) {
        error = "annotation frame buffer is shorter than its dimensions and stride";
        return false;
    }
    return true;
}

bool valid_options(const AnnotationOptions& options, std::string& error) {
    constexpr int max_opencv_thickness = 32767;
    constexpr double max_font_scale = 1000.0;
    if (options.box_thickness <= 0 ||
        options.box_thickness > max_opencv_thickness ||
        !std::isfinite(options.font_scale) || options.font_scale <= 0.0 ||
        options.font_scale > max_font_scale ||
        (options.show_metrics && (!std::isfinite(options.fps) ||
                                  !std::isfinite(options.inference_ms) ||
                                  options.fps < 0.0 || options.inference_ms < 0.0))) {
        error = "annotation style or metrics are invalid";
        return false;
    }
    return true;
}

std::ostringstream text_stream() {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2);
    return stream;
}

void draw_label(cv::Mat& canvas, int left, int top, int bottom,
                const std::string& label, double font_scale,
                const cv::Scalar& color) {
    int baseline = 0;
    const auto text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                           font_scale, 1, &baseline);
    const int label_width = std::min(canvas.cols - left, text_size.width + 4);
    const int label_height = std::min(canvas.rows,
                                      text_size.height + baseline + 4);
    const int label_top = top >= label_height
                              ? top - label_height
                              : std::clamp(bottom + 1, 0,
                                           canvas.rows - label_height);
    cv::rectangle(canvas, cv::Rect(left, label_top, label_width, label_height),
                  color, cv::FILLED);
    cv::putText(canvas, label,
                cv::Point(left + 2, label_top + text_size.height + 2),
                cv::FONT_HERSHEY_SIMPLEX, font_scale, cv::Scalar(255, 255, 255),
                1, cv::LINE_AA);
}

void draw_metrics(cv::Mat& canvas, const AnnotationOptions& options) {
    auto stream = text_stream();
    stream << "FPS " << std::setprecision(1) << options.fps << "  infer "
           << options.inference_ms << " ms";
    const auto label = stream.str();
    int baseline = 0;
    const auto text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                           options.font_scale, 1, &baseline);
    const int width = std::min(canvas.cols, text_size.width + 8);
    const int height = std::min(canvas.rows, text_size.height + baseline + 8);
    cv::rectangle(canvas, cv::Rect(0, 0, width, height),
                  cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(canvas, label, cv::Point(4, text_size.height + 4),
                cv::FONT_HERSHEY_SIMPLEX, options.font_scale,
                cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
}

} // namespace

std::string detection_label(const Detection& detection,
                            const std::vector<std::string>& class_names) {
    const bool named = detection.class_id >= 0 &&
                       static_cast<std::size_t>(detection.class_id) <
                           class_names.size() &&
                       !class_names[detection.class_id].empty();
    const std::string name = named ? class_names[detection.class_id]
                                   : "class_" + std::to_string(detection.class_id);
    auto stream = text_stream();
    stream << name << ' ' << detection.confidence;
    return stream.str();
}

bool annotate_frame(const Frame& source, const DetectionResult& detections,
                    const std::vector<std::string>& class_names,
                    const AnnotationOptions& options, Frame& output,
                    std::string& error) {
    error.clear();
    if (&source == &output) {
        error = "annotation output must be a distinct frame";
        return false;
    }
    output = {};
    if (!valid_frame(source, error)) return false;
    output = source;
    if (!options.enabled) return true;
    if (source.sequence != detections.frame_sequence) {
        error = "annotation frame and detection sequence do not match";
        output = {};
        return false;
    }
    if (!valid_options(options, error)) {
        output = {};
        return false;
    }

    try {
        cv::Mat canvas(output.height, output.width, CV_8UC3, output.bgr.data(),
                       static_cast<std::size_t>(output.stride));
        const cv::Scalar color(0, 255, 0); // BGR green
        for (const auto& detection : detections.detections) {
            if (detection.class_id < 0 || !std::isfinite(detection.confidence) ||
                !std::isfinite(detection.x1) || !std::isfinite(detection.y1) ||
                !std::isfinite(detection.x2) || !std::isfinite(detection.y2) ||
                detection.x2 <= detection.x1 || detection.y2 <= detection.y1 ||
                detection.x2 <= 0 || detection.y2 <= 0 ||
                detection.x1 >= output.width || detection.y1 >= output.height) {
                continue;
            }
            const int left = static_cast<int>(std::clamp(
                std::floor(static_cast<double>(detection.x1)), 0.0,
                static_cast<double>(output.width - 1)));
            const int top = static_cast<int>(std::clamp(
                std::floor(static_cast<double>(detection.y1)), 0.0,
                static_cast<double>(output.height - 1)));
            const int right = static_cast<int>(std::clamp(
                std::ceil(static_cast<double>(detection.x2)) - 1.0, 0.0,
                static_cast<double>(output.width - 1)));
            const int bottom = static_cast<int>(std::clamp(
                std::ceil(static_cast<double>(detection.y2)) - 1.0, 0.0,
                static_cast<double>(output.height - 1)));
            if (right < left || bottom < top) continue;
            cv::rectangle(canvas, cv::Point(left, top), cv::Point(right, bottom),
                          color, options.box_thickness, cv::LINE_8);
            draw_label(canvas, left, top, bottom,
                       detection_label(detection, class_names), options.font_scale,
                       color);
        }
        if (options.show_metrics) draw_metrics(canvas, options);
    } catch (const cv::Exception& exception) {
        error = std::string("OpenCV annotation failed: ") + exception.what();
        output = {};
        return false;
    }
    return true;
}

} // namespace skai
