#include "skai/inference/inference_backend.hpp"
#if SKAI_HAS_YOLO_PIPELINE
#include "skai/inference/yolo_detector.hpp"
#endif

namespace skai {
#if SKAI_HAS_YOLO_PIPELINE
namespace {
class TensorRtBackend final : public InferenceBackend {
public:
    TensorRtBackend(Logger& logger, const DetectorConfig& config)
        : bootstrap_(logger), detector_(logger, bootstrap_,
            {static_cast<float>(config.confidence), static_cast<float>(config.nms), 300}) {}
    bool load(const std::string& path, std::string& error) override {
        return detector_.load(path, error);
    }
    bool run(const BgrImageView& image, std::uint64_t sequence,
             DetectionResult& result, InferenceTiming& timing, std::string& error) override {
        return detector_.run(image, sequence, result, timing, error);
    }
private:
    TensorRtBootstrap bootstrap_;
    YoloDetector detector_;
};
} // namespace
#endif

std::unique_ptr<InferenceBackend> make_inference_backend(
        Logger& logger, const DetectorConfig& config) {
#if SKAI_HAS_YOLO_PIPELINE
    return std::make_unique<TensorRtBackend>(logger, config);
#else
    return nullptr;
#endif
}

bool inference_backend_available() noexcept {
#if SKAI_HAS_YOLO_PIPELINE
    return true;
#else
    return false;
#endif
}
} // namespace skai
