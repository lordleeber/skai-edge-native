#pragma once

#include <functional>
#include <mutex>
#include <ostream>
#include <string>

namespace skai {

enum class LogLevel { Trace, Debug, Info, Warning, Error };

const char* log_level_name(LogLevel level);

class Logger {
public:
    using ErrorSink = std::function<void(const std::string&, const std::string&)>;

    explicit Logger(std::ostream& output, LogLevel minimum_level = LogLevel::Info);
    void set_minimum_level(LogLevel level);
    void set_error_sink(ErrorSink sink);
    void log(LogLevel level, const std::string& module, const std::string& message);

private:
    std::ostream& output_;
    LogLevel minimum_level_;
    ErrorSink error_sink_;
    std::mutex mutex_;
};

} // namespace skai
