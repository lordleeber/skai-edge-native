#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <ostream>

namespace skai {

// Cumulative successful work, not a rolling percentile or freshness signal.
struct TimingSummary {
    std::uint64_t count = 0;
    double total_ms = 0, min_ms = 0, max_ms = 0;
    void observe(double ms) {
        if (!std::isfinite(ms) || ms < 0) return;
        if (!count || ms < min_ms) min_ms = ms;
        if (!count || ms > max_ms) max_ms = ms;
        ++count;
        total_ms += ms;
    }
};

inline void write_timing_json(std::ostream& output, const TimingSummary& timing) {
    output << "{\"count\":" << timing.count << ",\"total_ms\":" << timing.total_ms
           << ",\"mean_ms\":";
    if (timing.count) output << timing.total_ms / timing.count;
    else output << "null";
    output << ",\"min_ms\":";
    if (timing.count) output << timing.min_ms;
    else output << "null";
    output << ",\"max_ms\":";
    if (timing.count) output << timing.max_ms;
    else output << "null";
    output << '}';
}

enum class ProfileStage {
    PreprocessWall, PreprocessGpu, InferenceWall, InferenceGpu,
    PostprocessWall, AnnotationWall, RecordingQueueWait, Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(ProfileStage::Count)>
    profile_stage_names{{"preprocess_wall", "preprocess_gpu", "inference_wall",
        "inference_gpu", "postprocess_wall", "annotation_wall", "recording_queue_wait"}};
using ProfileSnapshot = std::array<TimingSummary, profile_stage_names.size()>;

class ProfileMetrics {
public:
    void observe(ProfileStage stage, double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto index = static_cast<std::size_t>(stage);
        if (index < stages_.size()) stages_[index].observe(ms);
    }
    ProfileSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stages_;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        stages_ = {};
    }
private:
    mutable std::mutex mutex_;
    ProfileSnapshot stages_{};
};

} // namespace skai
