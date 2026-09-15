#pragma once

#include "skai/inference/preprocess.hpp"
#include "skai/inference/tensorrt_engine.hpp"

#include <memory>
#include <string>

namespace skai {

struct PreprocessTiming {
    double gpu_ms = 0.0;  // CUDA event time: upload and fused conversion
    double wall_ms = 0.0; // Host time including scheduling and completion
};

// One inference worker owns one instance. run() waits for completion so the
// returned timing and TensorRT input buffer are ready on return.
class CudaPreprocessor {
public:
    CudaPreprocessor();
    ~CudaPreprocessor();
    CudaPreprocessor(const CudaPreprocessor&) = delete;
    CudaPreprocessor& operator=(const CudaPreprocessor&) = delete;

    bool run(const BgrImageView& image, TensorRtEngine& engine,
             const std::string& input_name, PreprocessPlan& plan,
             PreprocessTiming& timing, std::string& error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace skai
