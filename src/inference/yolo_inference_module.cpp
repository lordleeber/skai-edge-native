#include "skai/inference/yolo_inference_module.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
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

std::string detection_event_data(const DetectionResult& result, const Frame& frame) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10)
           << "{\"available\":true,\"frame_sequence\":" << result.frame_sequence
           << ",\"pts_ns\":" << frame.pts_ns
           << ",\"frame_width\":" << frame.width
           << ",\"frame_height\":" << frame.height
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
                                         std::shared_ptr<AlertManager> alerts,
                                         InferenceBackendFactory backend_factory)
    : input_(input), output_(&annotated_output), logger_(logger),
      status_(std::move(status)), api_(std::move(api)),
      events_(std::move(events)), alerts_(std::move(alerts)),
      backend_factory_(std::move(backend_factory)) {}

YoloInferenceModule::YoloInferenceModule(
        BoundedQueue<Frame>& input, Logger& logger,
        std::shared_ptr<RuntimeStatus> status, std::shared_ptr<ApiState> api,
        std::shared_ptr<EventChannel> events, std::shared_ptr<AlertManager> alerts,
                                         InferenceBackendFactory backend_factory)
    : input_(input), logger_(logger), status_(std::move(status)),
      api_(std::move(api)), events_(std::move(events)), alerts_(std::move(alerts)),
      backend_factory_(std::move(backend_factory)) {}

bool YoloInferenceModule::initialize(const Config& config) {
    if (detector_ || worker_.joinable()) return false;
    input_.reset();
    if (output_) output_->reset();
    alert_queue_.reset();
    if (status_) status_->clear_detector();
    annotation_.enabled = config.detector.annotate;
    annotation_.show_metrics = true;
    if (alerts_) {
        alerts_->configure(config.alerts, config.storage.alert_directory,
                           std::filesystem::path(config.detector.engine)
                               .filename().string());
    }
    detector_ = backend_factory_ ? backend_factory_(logger_, config.detector) : nullptr;
    if (!detector_) {
        logger_.log(LogLevel::Error, "detector", "inference backend unavailable");
        return false;
    }
    std::string error;
    if (detector_->load(config.detector.engine, error)) return true;
    logger_.log(LogLevel::Error, "detector", error);
    detector_.reset();
    return false;
}

bool YoloInferenceModule::start() {
    if (!detector_ || worker_.joinable()) return false;
    stopping_ = false;
    if (alerts_) alert_worker_ = std::thread(&YoloInferenceModule::persist_alerts, this);
    worker_ = std::thread(&YoloInferenceModule::run, this);
    return true;
}

void YoloInferenceModule::stop() noexcept {
    stopping_ = true;
    input_.shutdown();
    alert_queue_.shutdown();
}

void YoloInferenceModule::wait() noexcept {
    if (worker_.joinable()) worker_.join();
    if (alert_worker_.joinable()) alert_worker_.join();
    if (output_) output_->shutdown();
    if (status_) status_->clear_detector();
    detector_.reset();
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
            if (output_) output_->push(std::move(*frame));
            continue;
        }
        const auto invalidate = [&] {
            if (api_) {
                api_->invalidate_detections(permit, [&](bool was_available) {
                    if (status_) status_->clear_detector();
                    if (was_available && events_) {
                        events_->publish(EventType::Detection,
                                         "{\"available\":false}");
                    }
                });
            } else if (status_) {
                status_->clear_detector();
            }
            previous = {};
        };
        DetectionResult detections;
        InferenceTiming timing;
        std::string error;
        const BgrImageView image{frame->bgr.data(), frame->bgr.size(), frame->width,
                                 frame->height, frame->stride};
        bool detected = false;
        try {
            detected = detector_->run(image, frame->sequence, detections,
                                      timing, error);
        } catch (const std::exception& failure) {
            error = failure.what();
        } catch (...) {
            error = "unknown TensorRT inference error";
        }
        if (!detected) {
            invalidate();
            logger_.log(LogLevel::Error, "detector", error);
            if (output_) output_->push(std::move(*frame));
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
        double annotation_ms = 0;
        const auto annotation_start = std::chrono::steady_clock::now();
        Frame annotated;
        if (output_) {
            if (!annotate_frame(*frame, detections, coco_class_names(), annotation_,
                                annotated, error)) {
                invalidate();
                logger_.log(LogLevel::Error, "annotation", error);
                if (output_) output_->push(std::move(*frame));
                continue;
            }
        }
        if (output_) annotation_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - annotation_start).count();
        std::vector<DetectionDto> published;
        if (api_ || detection_sink_) {
            published.reserve(detections.detections.size());
            for (const auto& detection : detections.detections) {
                published.push_back({detection.class_id, class_name(detection.class_id),
                                     detection.confidence,
                                     detection.x1, detection.y1, detection.x2,
                                     detection.y2});
            }
        }
        // commit_detections() takes `published` by value, so the sink keeps a copy.
        std::vector<DetectionDto> sink_detections;
        if (detection_sink_) sink_detections = published;
        std::optional<AlertWork> alert_work;
        if (alerts_) {
            alert_work = AlertWork{detections, output_ ? annotated : *frame,
                                   permit.generation};
        }
        auto commit = [&] {
            previous = now;
            if (status_) {
                status_->observe_profile(ProfileStage::PreprocessWall, timing.preprocess.wall_ms);
                status_->observe_profile(ProfileStage::PreprocessGpu, timing.preprocess.gpu_ms);
                status_->observe_profile(ProfileStage::InferenceWall, timing.inference_wall_ms);
                status_->observe_profile(ProfileStage::InferenceGpu, timing.inference_gpu_ms);
                status_->observe_profile(ProfileStage::PostprocessWall, timing.postprocess_wall_ms);
                if (output_) status_->observe_profile(ProfileStage::AnnotationWall, annotation_ms);
                if (has_previous) {
                    status_->update_detector(annotation_.fps,
                                             annotation_.inference_ms);
                } else {
                    status_->update_inference(annotation_.inference_ms);
                }
            }
            if (events_) {
                events_->publish(EventType::Detection,
                                 detection_event_data(detections, *frame));
            }
            if (detection_sink_) detection_sink_(*frame, sink_detections);
            if (alerts_) {
                const auto dropped = alert_queue_.stats().dropped;
                alert_queue_.push(std::move(*alert_work));
                if (alert_queue_.stats().dropped != dropped) {
                    logger_.log(LogLevel::Error, "alerts",
                                "alert persistence queue dropped its oldest frame");
                }
            }
            if (output_) output_->push(std::move(annotated));
        };
        if (!api_) {
            commit();
        } else if (!api_->commit_detections(
                       permit, detections.frame_sequence, frame->width, frame->height,
                       frame->pts_ns, std::move(published), commit)) {
            if (output_) output_->push(std::move(*frame));
        }
    }
}

void YoloInferenceModule::persist_alerts() noexcept {
    while (auto work = alert_queue_.pop()) {
        try {
            alerts_->process(work->detections, work->snapshot.width,
                             work->snapshot.height, coco_class_names(),
                             work->generation, &work->snapshot);
        } catch (const std::exception& failure) {
            logger_.log(LogLevel::Error, "alerts", failure.what());
        }
    }
}

} // namespace skai
