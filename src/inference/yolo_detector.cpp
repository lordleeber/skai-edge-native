#include "skai/inference/yolo_detector.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace skai {
namespace {

bool cuda_ok(cudaError_t status, const char* operation, std::string& error) {
    if (status == cudaSuccess) return true;
    error = std::string(operation) + ": " + cudaGetErrorString(status);
    return false;
}

bool valid_config(const YoloPostprocessConfig& config) {
    return std::isfinite(config.confidence_threshold) &&
           std::isfinite(config.nms_iou_threshold) &&
           config.confidence_threshold >= 0.0f &&
           config.confidence_threshold <= 1.0f &&
           config.nms_iou_threshold >= 0.0f &&
           config.nms_iou_threshold <= 1.0f &&
           config.max_detections > 0;
}

} // namespace

struct YoloDetector::State {
    ~State() {
        if (start) cudaEventDestroy(start);
        if (end) cudaEventDestroy(end);
    }
    std::unique_ptr<nvinfer1::IExecutionContext> context;
    cudaEvent_t start = nullptr;
    cudaEvent_t end = nullptr;
    std::vector<float> output;
    int channels = 0;
    int candidates = 0;
};

YoloDetector::YoloDetector(Logger& logger, TensorRtBootstrap& bootstrap,
                           YoloPostprocessConfig config)
    : bootstrap_(bootstrap), config_(config), engine_(logger) {}
YoloDetector::~YoloDetector() = default;

bool YoloDetector::load(const std::string& engine_path, std::string& error) {
    state_.reset();
    engine_.unload();
    error.clear();
    if (!valid_config(config_)) {
        error = "YOLO confidence/NMS threshold or max detections is invalid";
        return false;
    }
    if (!bootstrap_.initialize_standard_plugins(error)) return false;
    if (!engine_.load(engine_path, error)) return false;

    const auto& tensors = engine_.tensors();
    const auto input = std::find_if(tensors.begin(), tensors.end(),
                                    [](const TensorInfo& tensor) {
                                        return tensor.name == "images" && tensor.input;
                                    });
    const auto output = std::find_if(tensors.begin(), tensors.end(),
                                     [](const TensorInfo& tensor) {
                                         return tensor.name == "output0" && !tensor.input;
                                     });
    if (tensors.size() != 2 || input == tensors.end() || output == tensors.end() ||
        input->data_type != "FP32" || input->shape.size() != 4 ||
        input->shape[0] != 1 || input->shape[1] != 3 ||
        input->shape[2] <= 0 || input->shape[3] <= 0 ||
        input->shape[2] > std::numeric_limits<int>::max() ||
        input->shape[3] > std::numeric_limits<int>::max() ||
        output->data_type != "FP32" || output->shape.size() != 3 ||
        output->shape[0] != 1 || output->shape[1] <= 4 ||
        output->shape[2] <= 0 ||
        output->shape[1] > std::numeric_limits<int>::max() ||
        output->shape[2] > std::numeric_limits<int>::max() ||
        output->bytes / sizeof(float) !=
            static_cast<std::size_t>(output->shape[1]) * output->shape[2]) {
        error = "YOLO engine must have FP32 images [1,3,H,W] and "
                "output0 [1,4+classes,candidates]";
        engine_.unload();
        return false;
    }

    auto next = std::make_unique<State>();
    next->channels = static_cast<int>(output->shape[1]);
    next->candidates = static_cast<int>(output->shape[2]);
    next->output.resize(output->bytes / sizeof(float));
    next->context.reset(engine_.native_engine()->createExecutionContext());
    if (!next->context ||
        !next->context->setTensorAddress("images", engine_.device_buffer("images")) ||
        !next->context->setTensorAddress("output0", engine_.device_buffer("output0"))) {
        error = "cannot create or bind YOLO TensorRT execution context";
        next.reset();
        engine_.unload();
        return false;
    }
    if (!cuda_ok(cudaEventCreate(&next->start), "cannot create inference start event",
                 error) ||
        !cuda_ok(cudaEventCreate(&next->end), "cannot create inference end event",
                 error)) {
        next.reset();
        engine_.unload();
        return false;
    }
    state_ = std::move(next);
    return true;
}

bool YoloDetector::loaded() const noexcept { return static_cast<bool>(state_); }

bool YoloDetector::run(const BgrImageView& image, std::uint64_t frame_sequence,
                       DetectionResult& result, InferenceTiming& timing,
                       std::string& error) {
    result = {};
    timing = {};
    error.clear();
    if (!loaded()) {
        error = "YOLO engine is not loaded";
        return false;
    }
    PreprocessPlan plan;
    if (!preprocessor_.run(image, engine_, "images", plan, timing.preprocess,
                           error)) return false;

    const auto wall_start = std::chrono::steady_clock::now();
    const auto stream = engine_.stream();
    if (!cuda_ok(cudaEventRecord(state_->start, stream),
                 "cannot start inference timing", error)) return false;
    if (!state_->context->enqueueV3(stream)) {
        cudaStreamSynchronize(stream);
        error = "YOLO TensorRT enqueueV3 failed";
        return false;
    }
    if (!cuda_ok(cudaEventRecord(state_->end, stream),
                 "cannot stop inference timing", error)) {
        cudaStreamSynchronize(stream);
        return false;
    }
    if (!cuda_ok(cudaMemcpyAsync(state_->output.data(), engine_.device_buffer("output0"),
                                 state_->output.size() * sizeof(float),
                                 cudaMemcpyDeviceToHost, stream),
                 "cannot copy YOLO output to host", error)) {
        cudaStreamSynchronize(stream);
        return false;
    }
    if (!cuda_ok(cudaStreamSynchronize(stream), "YOLO inference did not complete",
                 error)) return false;
    float gpu_ms = 0.0f;
    if (!cuda_ok(cudaEventElapsedTime(&gpu_ms, state_->start, state_->end),
                 "cannot measure YOLO inference", error)) return false;
    timing.inference_gpu_ms = gpu_ms;
    timing.inference_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wall_start).count();

    const auto post_start = std::chrono::steady_clock::now();
    const bool success = postprocess_yolo(
        {state_->output.data(), state_->output.size(), state_->channels,
         state_->candidates}, plan, config_, frame_sequence, result, error);
    timing.postprocess_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - post_start).count();
    return success;
}

const TensorRtEngine& YoloDetector::engine() const noexcept { return engine_; }

} // namespace skai
