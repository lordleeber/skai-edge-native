#include "skai/inference/yolo_inference_module.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
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

std::string class_name(int class_id) {
    const auto& names = coco_class_names();
    return class_id >= 0 && static_cast<std::size_t>(class_id) < names.size()
               ? names[class_id]
               : "class_" + std::to_string(class_id);
}

std::string detection_event_data(const DetectionResult& result) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10)
           << "{\"available\":true,\"frame_sequence\":" << result.frame_sequence
           << ",\"detections\":[";
    for (std::size_t index = 0; index < result.detections.size(); ++index) {
        const auto& detection = result.detections[index];
        if (index) output << ',';
        output << "{\"class_id\":" << detection.class_id
               << ",\"class_name\":\"" << class_name(detection.class_id)
               << "\",\"confidence\":" << detection.confidence
               << ",\"box\":[" << detection.x1 << ',' << detection.y1 << ','
               << detection.x2 << ',' << detection.y2 << "]}";
    }
    output << "]}";
    return output.str();
}

} // namespace

YoloInferenceModule::YoloInferenceModule(BoundedQueue<Frame>& input,
                                         BoundedQueue<Frame>& annotated_output,
                                         Logger& logger,
                                         std::shared_ptr<RuntimeStatus> status,
                                         std::shared_ptr<ApiState> api,
                                         std::shared_ptr<EventChannel> events,
                                         std::shared_ptr<AlertManager> alerts)
    : input_(input), output_(annotated_output), logger_(logger),
      status_(std::move(status)), api_(std::move(api)),
      events_(std::move(events)), alerts_(std::move(alerts)) {}

bool YoloInferenceModule::initialize(const Config& config) {
    if (detector_ || worker_.joinable()) return false;
    input_.reset();
    output_.reset();
    if (status_) status_->clear_detector();
    annotation_.enabled = config.detector.annotate;
    annotation_.show_metrics = true;
    if (alerts_) {
        alerts_->configure(config.alerts, config.storage.alert_directory,
                           std::filesystem::path(config.detector.engine)
                               .filename().string());
    }
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
    if (status_) status_->clear_detector();
    detector_.reset();
    bootstrap_.reset();
}

void YoloInferenceModule::run() noexcept {
    auto previous = std::chrono::steady_clock::time_point{};
    while (!stopping_) {
        auto frame = input_.pop();
        if (!frame || stopping_) break;
        const auto permit = api_ ? api_->detector_permit()
                                 : DetectorPermit{true, 0};
        if (!permit.enabled) {
            if (status_) status_->clear_detector();
            output_.push(std::move(*frame));
            continue;
        }
        DetectionResult detections;
        InferenceTiming timing;
        std::string error;
        const BgrImageView image{frame->bgr.data(), frame->bgr.size(), frame->width,
                                 frame->height, frame->stride};
        if (!detector_->run(image, frame->sequence, detections, timing, error)) {
            if (status_) status_->clear_detector();
            logger_.log(LogLevel::Error, "detector", error);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool has_previous = previous != std::chrono::steady_clock::time_point{};
        annotation_.fps = has_previous
                              ? 1.0 / std::chrono::duration<double>(now - previous).count()
                              : 0.0;
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
        std::vector<DetectionDto> published;
        if (api_) {
            published.reserve(detections.detections.size());
            for (const auto& detection : detections.detections) {
                published.push_back({detection.class_id, class_name(detection.class_id),
                                     detection.confidence,
                                     detection.x1, detection.y1, detection.x2,
                                     detection.y2});
            }
        }
        auto commit = [&] {
            previous = now;
            if (status_) {
                if (has_previous) {
                    status_->update_detector(annotation_.fps,
                                             annotation_.inference_ms);
                } else {
                    status_->update_inference(annotation_.inference_ms);
                }
            }
            if (events_) {
                events_->publish(EventType::Detection,
                                 detection_event_data(detections));
            }
            if (alerts_) {
                alerts_->process(detections, frame->width, frame->height,
                                 coco_class_names(), permit.generation, &annotated);
            }
            output_.push(std::move(annotated));
        };
        if (!api_) {
            commit();
        } else if (!api_->commit_detections(permit, detections.frame_sequence,
                                            std::move(published), commit)) {
            output_.push(std::move(*frame));
        }
    }
}

} // namespace skai
