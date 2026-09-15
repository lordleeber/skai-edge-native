#pragma once

#include "skai/inference/tensor_layout.hpp"
#include "skai/logging.hpp"

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include <memory>
#include <string>
#include <vector>

namespace skai {

// Owns one deserialized engine, its I/O device allocations and a CUDA stream.
// The caller selects the CUDA device before load() and keeps that device active
// when releasing the object. Execution contexts belong to the inference stage.
class TensorRtEngine {
public:
    explicit TensorRtEngine(Logger& logger);
    ~TensorRtEngine();

    TensorRtEngine(const TensorRtEngine&) = delete;
    TensorRtEngine& operator=(const TensorRtEngine&) = delete;

    bool load(const std::string& path, std::string& error);
    void unload() noexcept;
    bool loaded() const noexcept;
    const std::vector<TensorInfo>& tensors() const noexcept;
    const std::string& engine_name() const noexcept;
    std::string tensorrt_version() const;
    void* device_buffer(const std::string& name) const noexcept;
    cudaStream_t stream() const noexcept;
    nvinfer1::ICudaEngine* native_engine() const noexcept;

private:
    struct State;
    Logger& logger_;
    std::unique_ptr<State> state_;
};

} // namespace skai
