#pragma once

#include "skai/config.hpp"
#include "skai/inference/yolo_postprocess.hpp"

#include <functional>
#include <memory>

namespace skai {

struct PreprocessTiming {
    double gpu_ms = 0.0;
    double wall_ms = 0.0;
};

struct InferenceTiming {
    PreprocessTiming preprocess;
    double inference_gpu_ms = 0.0;
    double inference_wall_ms = 0.0;
    double postprocess_wall_ms = 0.0;
};

// Inference only. The lifecycle module owns queues, events and persistence.
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    virtual bool load(const std::string& engine_path, std::string& error) = 0;
    virtual bool run(const BgrImageView& image, std::uint64_t frame_sequence,
                     DetectionResult& result, InferenceTiming& timing,
                     std::string& error) = 0;
};

using InferenceBackendFactory = std::function<std::unique_ptr<InferenceBackend>(
    Logger&, const DetectorConfig&)>;
std::unique_ptr<InferenceBackend> make_inference_backend(Logger&, const DetectorConfig&);
bool inference_backend_available() noexcept;

} // namespace skai
