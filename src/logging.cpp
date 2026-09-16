#include "skai/logging.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace skai {
namespace {

std::string escape(const std::string& value) {
    std::string result;
    for (const char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
    return result.str();
}

} // namespace

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "unknown";
}

Logger::Logger(std::ostream& output, LogLevel minimum_level)
    : output_(output), minimum_level_(minimum_level) {}

void Logger::set_minimum_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    minimum_level_ = level;
}

void Logger::set_error_sink(ErrorSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_sink_ = std::move(sink);
}

void Logger::log(LogLevel level, const std::string& module, const std::string& message) {
    ErrorSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < minimum_level_) return;
        output_ << "timestamp=\"" << timestamp() << "\" level="
                << log_level_name(level) << " module=" << module
                << " message=\"" << escape(message) << "\"\n" << std::flush;
        if (level == LogLevel::Error) sink = error_sink_;
    }
    if (sink) {
        try {
            sink(module, message);
        } catch (...) {
            // Logging must remain safe when an optional observer fails.
        }
    }
}

} // namespace skai
