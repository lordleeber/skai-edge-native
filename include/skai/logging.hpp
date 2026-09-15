#pragma once

#include <mutex>
#include <ostream>
#include <string>

namespace skai {

enum class LogLevel { Trace, Debug, Info, Warning, Error };

const char* log_level_name(LogLevel level);

class Logger {
public:
    explicit Logger(std::ostream& output, LogLevel minimum_level = LogLevel::Info);
    void set_minimum_level(LogLevel level);
    void log(LogLevel level, const std::string& module, const std::string& message);

private:
    std::ostream& output_;
    LogLevel minimum_level_;
    std::mutex mutex_;
};

} // namespace skai
