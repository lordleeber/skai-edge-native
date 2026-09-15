#pragma once

#include "skai/logging.hpp"

#include <NvInfer.h>

#include <mutex>
#include <string>

namespace skai::detail {

class TensorRtLogger final : public nvinfer1::ILogger {
public:
    explicit TensorRtLogger(Logger& logger) : logger_(logger) {}
    void log(Severity severity, const char* message) noexcept override;
    std::string last_error() const;

private:
    Logger& logger_;
    mutable std::mutex mutex_;
    std::string last_error_;
};

} // namespace skai::detail
