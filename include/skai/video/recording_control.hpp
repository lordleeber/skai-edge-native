#pragma once

#include "skai/config.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace skai {

struct RecordingStatus {
    bool configured = false;
    bool active = false;
    std::string state = "stopped";
    std::string current_path;
    std::string last_error;
    std::uint64_t access_units_written = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t access_units_dropped = 0;
};

class RecordingController {
public:
    void configure(const RecordingConfig& config);
    bool start(std::string& error);
    bool stop(std::string& error);
    bool requested() const;
    RecordingStatus status() const;
    void set_active(std::string path);
    void set_stopped();
    void set_error(std::string error);
    void commit_written(std::uint64_t access_units, std::uint64_t bytes);
    void add_dropped();

private:
    mutable std::mutex mutex_;
    bool configured_ = false;
    bool requested_ = false;
    RecordingStatus status_;
};

} // namespace skai
