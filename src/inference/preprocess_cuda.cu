#include "skai/inference/preprocess_cuda.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>

namespace skai {
namespace {

__device__ float channel_at(const unsigned char* source, int stride,
                            int x, int y, int channel) {
    return static_cast<float>(source[static_cast<std::size_t>(y) * stride +
                                     static_cast<std::size_t>(x) * 3 + channel]);
}

__device__ float bilinear_channel(const unsigned char* source, int stride,
                                  int source_width, int source_height,
                                  float sx, float sy, int channel) {
    const int floor_x = static_cast<int>(floorf(sx));
    const int floor_y = static_cast<int>(floorf(sy));
    const float wx = sx - static_cast<float>(floor_x);
    const float wy = sy - static_cast<float>(floor_y);
    const int x0 = max(0, min(floor_x, source_width - 1));
    const int x1 = max(0, min(floor_x + 1, source_width - 1));
    const int y0 = max(0, min(floor_y, source_height - 1));
    const int y1 = max(0, min(floor_y + 1, source_height - 1));
    const float upper = channel_at(source, stride, x0, y0, channel) * (1.0f - wx) +
                        channel_at(source, stride, x1, y0, channel) * wx;
    const float lower = channel_at(source, stride, x0, y1, channel) * (1.0f - wx) +
                        channel_at(source, stride, x1, y1, channel) * wx;
    return upper * (1.0f - wy) + lower * wy;
}

__global__ void preprocess_kernel(const unsigned char* source, int stride,
                                  PreprocessPlan plan, float* output) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= plan.width || y >= plan.height) return;
    const auto plane = static_cast<std::size_t>(plan.width) * plan.height;
    const auto offset = static_cast<std::size_t>(y) * plan.width + x;
    if (x < plan.pad_left || x >= plan.pad_left + plan.resized_width ||
        y < plan.pad_top || y >= plan.pad_top + plan.resized_height) {
        const float pad = 114.0f / 255.0f;
        output[offset] = pad;
        output[plane + offset] = pad;
        output[2 * plane + offset] = pad;
        return;
    }
    const float sx = (static_cast<float>(x - plan.pad_left) + 0.5f) *
                         plan.source_width / plan.resized_width - 0.5f;
    const float sy = (static_cast<float>(y - plan.pad_top) + 0.5f) *
                         plan.source_height / plan.resized_height - 0.5f;
    output[offset] = bilinear_channel(source, stride, plan.source_width,
                                      plan.source_height, sx, sy, 2) / 255.0f;
    output[plane + offset] = bilinear_channel(source, stride, plan.source_width,
                                              plan.source_height, sx, sy, 1) / 255.0f;
    output[2 * plane + offset] = bilinear_channel(source, stride, plan.source_width,
                                                  plan.source_height, sx, sy, 0) / 255.0f;
}

bool cuda_ok(cudaError_t status, const char* operation, std::string& error) {
    if (status == cudaSuccess) return true;
    error = std::string(operation) + ": " + cudaGetErrorString(status);
    return false;
}

} // namespace

struct CudaPreprocessor::State {
    ~State() {
        if (staging) cudaFree(staging);
        if (start) cudaEventDestroy(start);
        if (end) cudaEventDestroy(end);
    }
    unsigned char* staging = nullptr;
    std::size_t staging_bytes = 0;
    cudaEvent_t start = nullptr;
    cudaEvent_t end = nullptr;
};

CudaPreprocessor::CudaPreprocessor() : state_(std::make_unique<State>()) {}
CudaPreprocessor::~CudaPreprocessor() = default;

bool CudaPreprocessor::run(const BgrImageView& image, TensorRtEngine& engine,
                           const std::string& input_name, PreprocessPlan& plan,
                           PreprocessTiming& timing, std::string& error) {
    plan = {};
    timing = {};
    error.clear();
    if (!engine.loaded()) {
        error = "TensorRT engine is not loaded";
        return false;
    }
    const auto& tensors = engine.tensors();
    const auto binding = std::find_if(tensors.begin(), tensors.end(),
                                      [&](const TensorInfo& tensor) {
                                          return tensor.name == input_name && tensor.input;
                                      });
    if (binding == tensors.end()) {
        error = "TensorRT input tensor '" + input_name + "' is unavailable";
        return false;
    }
    if (binding->data_type != "FP32" || binding->shape.size() != 4 ||
        binding->shape[0] != 1 || binding->shape[1] != 3 ||
        binding->shape[2] > std::numeric_limits<int>::max() ||
        binding->shape[3] > std::numeric_limits<int>::max()) {
        error = "TensorRT input '" + input_name + "' must be FP32 NCHW [1,3,H,W]";
        return false;
    }
    if (!make_preprocess_plan(image, static_cast<int>(binding->shape[3]),
                              static_cast<int>(binding->shape[2]), plan, error)) {
        return false;
    }
    auto* output = static_cast<float*>(engine.device_buffer(input_name));
    const auto stream = engine.stream();
    if (!output || !stream) {
        error = "TensorRT input buffer or CUDA stream is unavailable";
        return false;
    }
    const auto stride = static_cast<std::size_t>(image.stride);
    const auto height = static_cast<std::size_t>(image.height);
    if (height > std::numeric_limits<std::size_t>::max() / stride) {
        error = "BGR upload size overflow";
        return false;
    }
    const auto required = stride * height;
    if (required > state_->staging_bytes) {
        if (state_->staging) cudaFree(state_->staging);
        state_->staging = nullptr;
        state_->staging_bytes = 0;
        if (!cuda_ok(cudaMalloc(reinterpret_cast<void**>(&state_->staging), required),
                     "cannot allocate BGR CUDA staging buffer", error)) {
            return false;
        }
        state_->staging_bytes = required;
    }
    if (!state_->start &&
        !cuda_ok(cudaEventCreate(&state_->start), "cannot create CUDA start event", error)) {
        return false;
    }
    if (!state_->end &&
        !cuda_ok(cudaEventCreate(&state_->end), "cannot create CUDA end event", error)) {
        return false;
    }

    const auto wall_start = std::chrono::steady_clock::now();
    if (!cuda_ok(cudaEventRecord(state_->start, stream), "cannot start CUDA timing", error)) {
        return false;
    }
    if (!cuda_ok(cudaMemcpy2DAsync(state_->staging, image.stride, image.data,
                                   image.stride, static_cast<std::size_t>(image.width) * 3,
                                   image.height, cudaMemcpyHostToDevice, stream),
                 "cannot upload BGR frame", error)) {
        cudaStreamSynchronize(stream);
        return false;
    }
    constexpr dim3 block(16, 16);
    const dim3 grid((plan.width + block.x - 1) / block.x,
                    (plan.height + block.y - 1) / block.y);
    preprocess_kernel<<<grid, block, 0, stream>>>(state_->staging, image.stride,
                                                  plan, output);
    if (!cuda_ok(cudaGetLastError(), "cannot launch CUDA preprocessing", error)) {
        cudaStreamSynchronize(stream);
        return false;
    }
    if (!cuda_ok(cudaEventRecord(state_->end, stream), "cannot stop CUDA timing", error)) {
        cudaStreamSynchronize(stream);
        return false;
    }
    if (!cuda_ok(cudaEventSynchronize(state_->end),
                 "CUDA preprocessing did not complete", error)) {
        cudaStreamSynchronize(stream);
        return false;
    }
    float gpu_ms = 0.0f;
    if (!cuda_ok(cudaEventElapsedTime(&gpu_ms, state_->start, state_->end),
                 "cannot measure CUDA preprocessing", error)) {
        return false;
    }
    timing.gpu_ms = gpu_ms;
    timing.wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wall_start).count();
    return true;
}

} // namespace skai
