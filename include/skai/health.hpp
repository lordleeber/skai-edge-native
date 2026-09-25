#pragma once

#include "skai/gps/gps_source.hpp"
#include "skai/metrics.hpp"
#include "skai/status.hpp"
#include "skai/video/recording_control.hpp"
#include "skai/video/rtsp_metrics.hpp"
#include "skai/webrtc/webrtc_manager.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace skai {

enum class HealthState { Starting, Running, Degraded, Stopping, Failed };
enum class HealthComponent { VideoSource, Detector, Encoder, Recorder, Gps, Web,
                             WebRtc, Database };
inline constexpr std::array<HealthComponent, 8> all_health_components = {
    HealthComponent::VideoSource, HealthComponent::Detector, HealthComponent::Encoder,
    HealthComponent::Recorder, HealthComponent::Gps, HealthComponent::Web,
    HealthComponent::WebRtc, HealthComponent::Database};

const char* health_state_name(HealthState state);
const char* health_component_name(HealthComponent component);

struct ComponentHealth {
    HealthState state = HealthState::Starting;
    std::string detail;
};

struct HealthSnapshot {
    HealthState state = HealthState::Starting;
    std::array<ComponentHealth, 8> components{};
};

ComponentHealth video_health(const std::optional<RtspDiagnostics>& diagnostics,
                             int stall_timeout_ms = 3000);
ComponentHealth detector_health(bool supported, bool enabled,
                                const RuntimeStatusSnapshot& status);
ComponentHealth encoder_health(const RuntimeStatusSnapshot& status, bool media_available,
                               bool h264_required = true);
ComponentHealth recorder_health(const RecordingStatus& status, bool requested,
                                bool enabled_by_config);
ComponentHealth gps_health(bool enabled, const std::optional<GpsFix>& fix);
ComponentHealth webrtc_health(const WebRtcDiagnostics& diagnostics,
                              const WhipMetrics& whip = {});
ComponentHealth web_health(bool serving);
ComponentHealth database_health(const std::string& error);

// Polls component probes independently. A stale poll is a failed watchdog.
class HealthWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    using Probe = std::function<ComponentHealth()>;
    ~HealthWatchdog();

    void set_probe(HealthComponent component, Probe probe, bool critical = false);
    void set_lifecycle(HealthState state);
    void tick(Clock::time_point now = Clock::now());
    HealthSnapshot snapshot(Clock::time_point now = Clock::now()) const;
    void start();
    void stop();

private:
    struct Entry {
        Probe probe;
        ComponentHealth health;
        bool critical = false;
    };
    std::array<Entry, 8> entries_{};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    Clock::time_point last_poll_{};
    HealthState lifecycle_ = HealthState::Starting;
    bool stopping_ = false;
};

} // namespace skai
