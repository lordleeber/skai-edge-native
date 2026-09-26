#pragma once

#include "skai/inference/preprocess_cuda.hpp"
#include "skai/inference/yolo_postprocess.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace skai {

// One worker owns one detector; the process-level bootstrap must outlive it.
class YoloDetector {
public:
    YoloDetector(Logger& logger, TensorRtBootstrap& bootstrap,
                 YoloPostprocessConfig config = {});
    ~YoloDetector();
    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;

    bool load(const std::string& engine_path, std::string& error);
    bool loaded() const noexcept;
    bool run(const BgrImageView& image, std::uint64_t frame_sequence,
             DetectionResult& result, InferenceTiming& timing,
             std::string& error);
    const TensorRtEngine& engine() const noexcept;

private:
    struct State;
    TensorRtBootstrap& bootstrap_;
    YoloPostprocessConfig config_;
    TensorRtEngine engine_;
    CudaPreprocessor preprocessor_;
    std::unique_ptr<State> state_;
};

} // namespace skai
