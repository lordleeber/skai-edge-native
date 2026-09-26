#include "skai/metrics.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <fstream>
#include <system_error>

namespace skai {

SystemMetrics SystemMetricsSampler::sample() {
    std::lock_guard<std::mutex> lock(mutex_);
    SystemMetrics result;
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0, resident = 0;
    if (statm >> pages >> resident) {
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0) result.memory_rss_bytes = resident * page_size;
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        const double cpu_seconds = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
            (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1'000'000.0;
        const auto now = std::chrono::steady_clock::now();
        if (previous_wall_) {
            const double elapsed = std::chrono::duration<double>(now - *previous_wall_).count();
            if (elapsed > 0) {
                result.cpu_percent = 100.0 *
                    (cpu_seconds - previous_cpu_seconds_) / elapsed;
            }
        }
        previous_wall_ = now;
        previous_cpu_seconds_ = cpu_seconds;
    }
    for (const auto* device : {"/sys/devices/platform/17000000.gpu/load",
            "/sys/devices/platform/gpu.0/load", "/sys/devices/platform/bus@0/gpu.0/load"}) {
        std::ifstream gpu_load(device);
        int permille = -1;
        if (gpu_load >> permille && permille >= 0 && permille <= 1000) {
            result.gpu_percent = permille / 10.0;
            break;
        }
    }
    std::error_code error;
    auto path = std::filesystem::absolute(disk_path_, error);
    if (error) return result;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        path = path.parent_path();
        error.clear();
    }
    if (!path.empty()) {
        const auto space = std::filesystem::space(path, error);
        if (!error) result.disk_free_bytes = space.available;
    }
    return result;
}

} // namespace skai
