#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace skai {

struct RuntimeStatusSnapshot {
    std::string status;
    std::uint64_t uptime_s = 0;
    std::optional<double> video_fps;
    std::optional<double> detector_fps;
    std::optional<double> last_inference_ms;
};

// Lock-free cross-module metrics for the control plane. Writers publish a
// value before its availability flag, so readers never mistake defaults for
// measurements.
class RuntimeStatus {
public:
    void set_running(bool running) noexcept { running_.store(running); }

    void set_detector_expected(bool expected) noexcept {
        detector_expected_.store(expected);
        if (!expected) detector_available_.store(false);
    }

    void update_video(double fps) noexcept {
        video_fps_.store(fps);
        video_available_.store(true, std::memory_order_release);
    }

    void update_detector(double fps, double inference_ms) noexcept {
        detector_fps_.store(fps);
        inference_ms_.store(inference_ms);
        detector_fps_available_.store(true, std::memory_order_release);
        detector_available_.store(true, std::memory_order_release);
    }

    void update_inference(double inference_ms) noexcept {
        inference_ms_.store(inference_ms);
        detector_available_.store(true, std::memory_order_release);
    }

    void clear_video() noexcept { video_available_.store(false); }
    void clear_detector() noexcept {
        detector_available_.store(false);
        detector_fps_available_.store(false);
    }

    RuntimeStatusSnapshot snapshot() const {
        RuntimeStatusSnapshot result;
        const bool running = running_.load();
        const bool video = video_available_.load(std::memory_order_acquire);
        const bool detector = detector_available_.load(std::memory_order_acquire);
        const bool detector_expected = detector_expected_.load();
        if (!running) result.status = "starting";
        else if (video && (!detector_expected || detector)) result.status = "running";
        else result.status = "degraded";
        if (video) result.video_fps = video_fps_.load();
        if (detector) {
            if (detector_fps_available_.load(std::memory_order_acquire)) {
                result.detector_fps = detector_fps_.load();
            }
            result.last_inference_ms = inference_ms_.load();
        }
        return result;
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> detector_expected_{false};
    std::atomic<bool> video_available_{false};
    std::atomic<bool> detector_available_{false};
    std::atomic<bool> detector_fps_available_{false};
    std::atomic<double> video_fps_{0.0};
    std::atomic<double> detector_fps_{0.0};
    std::atomic<double> inference_ms_{0.0};
};

} // namespace skai
