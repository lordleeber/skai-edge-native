#pragma once

#include "skai/alerts/alert_manager.hpp"
#include "skai/application.hpp"
#include "skai/api_state.hpp"
#include "skai/core/bounded_queue.hpp"
#include "skai/events.hpp"
#include "skai/inference/yolo_detector.hpp"
#include "skai/status.hpp"
#include "skai/video/annotator.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace skai {

// Consumes decoded frames, runs YOLO, and publishes copied annotated frames for
// the next media stage. One worker owns all TensorRT/CUDA inference resources.
class YoloInferenceModule final : public LifecycleModule {
public:
    YoloInferenceModule(BoundedQueue<Frame>& input,
                        BoundedQueue<Frame>& annotated_output, Logger& logger,
                        std::shared_ptr<RuntimeStatus> status = {},
                        std::shared_ptr<ApiState> api = {},
                        std::shared_ptr<EventChannel> events = {},
                        std::shared_ptr<AlertManager> alerts = {});

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

private:
    struct AlertWork {
        DetectionResult detections;
        Frame snapshot;
        std::uint64_t generation = 0;
    };
    void run() noexcept;
    void persist_alerts() noexcept;

    BoundedQueue<Frame>& input_;
    BoundedQueue<Frame>& output_;
    Logger& logger_;
    std::shared_ptr<RuntimeStatus> status_;
    std::shared_ptr<ApiState> api_;
    std::shared_ptr<EventChannel> events_;
    std::shared_ptr<AlertManager> alerts_;
    BoundedQueue<AlertWork> alert_queue_{2};
    AnnotationOptions annotation_;
    std::unique_ptr<TensorRtBootstrap> bootstrap_;
    std::unique_ptr<YoloDetector> detector_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::thread alert_worker_;
};

} // namespace skai
