#include "skai/inference/yolo_inference_module.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace skai {
namespace {

const std::vector<std::string>& coco_class_names() {
    static const std::vector<std::string> names = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
        "truck", "boat", "traffic light", "fire hydrant", "stop sign",
        "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
        "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
        "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
        "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
        "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
        "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
        "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
        "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop",
        "mouse", "remote", "keyboard", "cell phone", "microwave", "oven",
        "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors",
        "teddy bear", "hair drier", "toothbrush"};
    return names;
}

} // namespace

YoloInferenceModule::YoloInferenceModule(BoundedQueue<Frame>& input,
                                         BoundedQueue<Frame>& annotated_output,
                                         Logger& logger)
    : input_(input), output_(annotated_output), logger_(logger) {}

bool YoloInferenceModule::initialize(const Config& config) {
    if (detector_ || worker_.joinable()) return false;
    input_.reset();
    output_.reset();
    annotation_.enabled = config.detector.annotate;
    annotation_.show_metrics = true;
    YoloPostprocessConfig postprocess;
    postprocess.confidence_threshold = static_cast<float>(config.detector.confidence);
    postprocess.nms_iou_threshold = static_cast<float>(config.detector.nms);
    bootstrap_ = std::make_unique<TensorRtBootstrap>(logger_);
    detector_ = std::make_unique<YoloDetector>(logger_, *bootstrap_, postprocess);
    std::string error;
    if (detector_->load(config.detector.engine, error)) return true;
    logger_.log(LogLevel::Error, "detector", error);
    detector_.reset();
    bootstrap_.reset();
    return false;
}

bool YoloInferenceModule::start() {
    if (!detector_ || worker_.joinable()) return false;
    stopping_ = false;
    worker_ = std::thread(&YoloInferenceModule::run, this);
    return true;
}

void YoloInferenceModule::stop() noexcept {
    stopping_ = true;
    input_.shutdown();
}

void YoloInferenceModule::wait() noexcept {
    if (worker_.joinable()) worker_.join();
    output_.shutdown();
    detector_.reset();
    bootstrap_.reset();
}

void YoloInferenceModule::run() noexcept {
    auto previous = std::chrono::steady_clock::time_point{};
    while (!stopping_) {
        auto frame = input_.pop();
        if (!frame || stopping_) break;
        DetectionResult detections;
        InferenceTiming timing;
        std::string error;
        const BgrImageView image{frame->bgr.data(), frame->bgr.size(), frame->width,
                                 frame->height, frame->stride};
        if (!detector_->run(image, frame->sequence, detections, timing, error)) {
            logger_.log(LogLevel::Error, "detector", error);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        annotation_.fps = previous == std::chrono::steady_clock::time_point{}
                              ? 0.0
                              : 1.0 / std::chrono::duration<double>(now - previous).count();
        previous = now;
        annotation_.inference_ms = timing.preprocess.wall_ms +
                                   timing.inference_wall_ms +
                                   timing.postprocess_wall_ms;
        Frame annotated;
        if (!annotate_frame(*frame, detections, coco_class_names(), annotation_,
                            annotated, error)) {
            logger_.log(LogLevel::Error, "annotation", error);
            continue;
        }
        output_.push(std::move(annotated));
    }
}

} // namespace skai
