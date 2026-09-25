#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace skai {

struct RuntimeStatusSnapshot {
    std::string status;
    std::uint64_t uptime_s = 0;
    std::optional<double> video_fps;
    std::optional<double> detector_fps;
    std::optional<double> last_inference_ms;
    std::optional<std::int64_t> last_inference_age_ms;
    struct Encoder {
        std::uint64_t frames_submitted = 0;
        std::uint64_t frames_rejected = 0;
        std::uint64_t frames_dropped = 0;
        std::uint64_t appsrc_pressure_dropped = 0;
        std::uint64_t access_units_encoded = 0;
        std::uint64_t bytes_encoded = 0;
        std::uint64_t access_units_dropped = 0;
        std::int64_t last_access_unit_age_ms = -1;
        std::string last_error;
    };
    std::optional<Encoder> encoder;
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
        inference_at_ns_.store(now_ns());
        detector_fps_available_.store(true, std::memory_order_release);
        detector_available_.store(true, std::memory_order_release);
    }

    void update_inference(double inference_ms) noexcept {
        inference_ms_.store(inference_ms);
        inference_at_ns_.store(now_ns());
        detector_available_.store(true, std::memory_order_release);
    }

    void clear_video() noexcept { video_available_.store(false); }
    void clear_detector() noexcept {
        detector_available_.store(false);
        detector_fps_available_.store(false);
    }

    void update_encoder(const RuntimeStatusSnapshot::Encoder& encoder) {
        encoder_frames_submitted_.store(encoder.frames_submitted);
        encoder_frames_rejected_.store(encoder.frames_rejected);
        encoder_frames_dropped_.store(encoder.frames_dropped);
        encoder_appsrc_pressure_dropped_.store(encoder.appsrc_pressure_dropped);
        encoder_access_units_encoded_.store(encoder.access_units_encoded);
        encoder_bytes_encoded_.store(encoder.bytes_encoded);
        encoder_access_units_dropped_.store(encoder.access_units_dropped);
        encoder_last_access_unit_age_ms_.store(encoder.last_access_unit_age_ms);
        {
            std::lock_guard<std::mutex> lock(encoder_error_mutex_);
            encoder_last_error_ = encoder.last_error;
        }
        encoder_available_.store(true, std::memory_order_release);
    }

    void clear_encoder() noexcept { encoder_available_.store(false); }

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
            result.last_inference_age_ms = (now_ns() - inference_at_ns_.load()) / 1000000;
        }
        if (encoder_available_.load(std::memory_order_acquire)) {
            RuntimeStatusSnapshot::Encoder encoder;
            encoder.frames_submitted = encoder_frames_submitted_.load();
            encoder.frames_rejected = encoder_frames_rejected_.load();
            encoder.frames_dropped = encoder_frames_dropped_.load();
            encoder.appsrc_pressure_dropped = encoder_appsrc_pressure_dropped_.load();
            encoder.access_units_encoded = encoder_access_units_encoded_.load();
            encoder.bytes_encoded = encoder_bytes_encoded_.load();
            encoder.access_units_dropped = encoder_access_units_dropped_.load();
            encoder.last_access_unit_age_ms = encoder_last_access_unit_age_ms_.load();
            {
                std::lock_guard<std::mutex> lock(encoder_error_mutex_);
                encoder.last_error = encoder_last_error_;
            }
            result.encoder = std::move(encoder);
        }
        return result;
    }

private:
    static std::int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::atomic<bool> running_{false};
    std::atomic<bool> detector_expected_{false};
    std::atomic<bool> video_available_{false};
    std::atomic<bool> detector_available_{false};
    std::atomic<bool> detector_fps_available_{false};
    std::atomic<double> video_fps_{0.0};
    std::atomic<double> detector_fps_{0.0};
    std::atomic<double> inference_ms_{0.0};
    std::atomic<std::int64_t> inference_at_ns_{0};
    std::atomic<bool> encoder_available_{false};
    std::atomic<std::uint64_t> encoder_frames_submitted_{0};
    std::atomic<std::uint64_t> encoder_frames_rejected_{0};
    std::atomic<std::uint64_t> encoder_frames_dropped_{0};
    std::atomic<std::uint64_t> encoder_appsrc_pressure_dropped_{0};
    std::atomic<std::uint64_t> encoder_access_units_encoded_{0};
    std::atomic<std::uint64_t> encoder_bytes_encoded_{0};
    std::atomic<std::uint64_t> encoder_access_units_dropped_{0};
    std::atomic<std::int64_t> encoder_last_access_unit_age_ms_{-1};
    mutable std::mutex encoder_error_mutex_;
    std::string encoder_last_error_;
};

} // namespace skai
