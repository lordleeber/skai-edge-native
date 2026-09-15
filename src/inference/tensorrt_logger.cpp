#include "inference/tensorrt_logger.hpp"

namespace skai::detail {

void TensorRtLogger::log(Severity severity, const char* message) noexcept {
    try {
        LogLevel level = LogLevel::Debug;
        if (severity == Severity::kINTERNAL_ERROR || severity == Severity::kERROR) {
            level = LogLevel::Error;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_error_ = message ? message : "unknown TensorRT error";
            }
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

std::string TensorRtLogger::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

} // namespace skai::detail
