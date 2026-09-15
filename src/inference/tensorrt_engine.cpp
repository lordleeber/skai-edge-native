#include "skai/inference/tensorrt_engine.hpp"

#include <NvInferPlugin.h>

#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace skai {
namespace {

class TensorRtLogger final : public nvinfer1::ILogger {
public:
    explicit TensorRtLogger(Logger& logger) : logger_(logger) {}

    void log(Severity severity, const char* message) noexcept override {
        try {
            LogLevel level = LogLevel::Debug;
            if (severity == Severity::kINTERNAL_ERROR || severity == Severity::kERROR) {
                level = LogLevel::Error;
                last_error_ = message ? message : "unknown TensorRT error";
            } else if (severity == Severity::kWARNING) {
                level = LogLevel::Warning;
            } else if (severity == Severity::kINFO) {
                level = LogLevel::Info;
            }
            logger_.log(level, "tensorrt", message ? message : "unknown TensorRT message");
        } catch (...) {
            // TensorRT's logger contract is noexcept.
        }
    }

    const std::string& last_error() const noexcept { return last_error_; }

private:
    Logger& logger_;
    std::string last_error_;
};

std::pair<const char*, std::size_t> tensor_type(nvinfer1::DataType type) {
    using Type = nvinfer1::DataType;
    switch (type) {
    case Type::kFLOAT: return {"FP32", 4};
    case Type::kHALF: return {"FP16", 2};
    case Type::kINT8: return {"INT8", 1};
    case Type::kINT32: return {"INT32", 4};
    case Type::kBOOL: return {"BOOL", 1};
    case Type::kUINT8: return {"UINT8", 1};
    case Type::kFP8: return {"FP8", 1};
    case Type::kBF16: return {"BF16", 2};
    case Type::kINT64: return {"INT64", 8};
    default: return {"", 0}; // Packed INT4 needs a separate sizing rule.
    }
}

bool read_engine_file(const std::string& path, std::vector<char>& bytes,
                      std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "cannot open TensorRT engine '" + path + "'";
        return false;
    }
    const auto length = input.tellg();
    if (length <= 0) {
        error = "TensorRT engine '" + path + "' is empty or unreadable";
        return false;
    }
    if (static_cast<std::uint64_t>(length) >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = "TensorRT engine '" + path + "' is too large";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = "cannot read TensorRT engine '" + path + "'";
        return false;
    }
    return true;
}

} // namespace

struct TensorRtEngine::State {
    explicit State(Logger& logger) : trt_logger(logger) {}
    ~State() {
        if (stream) cudaStreamSynchronize(stream);
        for (void* buffer : buffers) cudaFree(buffer);
        if (stream) cudaStreamDestroy(stream);
    }

    TensorRtLogger trt_logger;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::vector<TensorInfo> tensors;
    std::vector<void*> buffers;
    std::string name;
    cudaStream_t stream = nullptr;
};

TensorRtEngine::TensorRtEngine(Logger& logger) : logger_(logger) {}
TensorRtEngine::~TensorRtEngine() = default;

bool TensorRtEngine::load(const std::string& path, std::string& error) {
    unload();
    error.clear();
    std::vector<char> bytes;
    if (!read_engine_file(path, bytes, error)) return false;

    auto next = std::make_unique<State>(logger_);
    if (!initLibNvInferPlugins(&next->trt_logger, "")) {
        error = "failed to register TensorRT plugins";
        return false;
    }
    next->runtime.reset(nvinfer1::createInferRuntime(next->trt_logger));
    if (!next->runtime) {
        error = "failed to create TensorRT runtime";
        return false;
    }
    next->engine.reset(next->runtime->deserializeCudaEngine(bytes.data(), bytes.size()));
    if (!next->engine) {
        error = "cannot deserialize TensorRT engine '" + path +
                "'; it may be corrupt or incompatible with this TensorRT version, GPU, or plugins";
        if (!next->trt_logger.last_error().empty()) {
            error += ": " + next->trt_logger.last_error();
        }
        return false;
    }

    std::vector<TensorDescription> descriptions;
    const auto count = next->engine->getNbIOTensors();
    descriptions.reserve(static_cast<std::size_t>(count > 0 ? count : 0));
    for (int index = 0; index < count; ++index) {
        const char* raw_name = next->engine->getIOTensorName(index);
        if (!raw_name) {
            error = "engine has an unnamed I/O tensor";
            return false;
        }
        TensorDescription description;
        description.name = raw_name;
        const auto mode = next->engine->getTensorIOMode(raw_name);
        if (mode != nvinfer1::TensorIOMode::kINPUT &&
            mode != nvinfer1::TensorIOMode::kOUTPUT) {
            error = "tensor '" + description.name + "' has no input/output mode";
            return false;
        }
        description.input = mode == nvinfer1::TensorIOMode::kINPUT;
        const auto dims = next->engine->getTensorShape(raw_name);
        if (dims.nbDims < 0 || dims.nbDims > nvinfer1::Dims::MAX_DIMS) {
            error = "tensor '" + description.name + "' has invalid dimensions";
            return false;
        }
        for (int dimension = 0; dimension < dims.nbDims; ++dimension) {
            description.shape.push_back(dims.d[dimension]);
        }
        const auto [type_name, type_bytes] = tensor_type(
            next->engine->getTensorDataType(raw_name));
        description.data_type = type_name;
        description.component_bytes = type_bytes;
        description.device = next->engine->getTensorLocation(raw_name) ==
                             nvinfer1::TensorLocation::kDEVICE;
        description.linear = next->engine->getTensorFormat(raw_name) ==
                                 nvinfer1::TensorFormat::kLINEAR &&
                             next->engine->getTensorVectorizedDim(raw_name) == -1;
        descriptions.push_back(std::move(description));
    }
    if (!validate_tensor_layout(descriptions, next->tensors, error)) {
        error = "incompatible TensorRT engine '" + path + "': " + error;
        return false;
    }

    const auto status = cudaStreamCreateWithFlags(&next->stream, cudaStreamNonBlocking);
    if (status != cudaSuccess) {
        error = "cannot create CUDA stream for engine '" + path + "': " +
                cudaGetErrorString(status);
        return false;
    }
    for (const auto& tensor : next->tensors) {
        void* buffer = nullptr;
        const auto allocation = cudaMalloc(&buffer, tensor.bytes);
        if (allocation != cudaSuccess) {
            error = "cannot allocate CUDA buffer for tensor '" + tensor.name +
                    "' (" + std::to_string(tensor.bytes) + " bytes): " +
                    cudaGetErrorString(allocation);
            return false;
        }
        next->buffers.push_back(buffer);
    }
    const char* raw_name = next->engine->getName();
    next->name = raw_name ? raw_name : "";
    state_ = std::move(next);
    return true;
}

void TensorRtEngine::unload() noexcept { state_.reset(); }
bool TensorRtEngine::loaded() const noexcept { return static_cast<bool>(state_); }
const std::vector<TensorInfo>& TensorRtEngine::tensors() const noexcept {
    static const std::vector<TensorInfo> empty;
    return state_ ? state_->tensors : empty;
}
const std::string& TensorRtEngine::engine_name() const noexcept {
    static const std::string empty;
    return state_ ? state_->name : empty;
}
std::string TensorRtEngine::tensorrt_version() const {
    return std::to_string(NV_TENSORRT_MAJOR) + "." +
           std::to_string(NV_TENSORRT_MINOR) + "." +
           std::to_string(NV_TENSORRT_PATCH);
}
void* TensorRtEngine::device_buffer(const std::string& name) const noexcept {
    if (!state_) return nullptr;
    for (std::size_t index = 0; index < state_->tensors.size(); ++index) {
        if (state_->tensors[index].name == name) return state_->buffers[index];
    }
    return nullptr;
}
cudaStream_t TensorRtEngine::stream() const noexcept {
    return state_ ? state_->stream : nullptr;
}
nvinfer1::ICudaEngine* TensorRtEngine::native_engine() const noexcept {
    return state_ ? state_->engine.get() : nullptr;
}

} // namespace skai
