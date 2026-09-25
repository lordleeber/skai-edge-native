#include "skai/health.hpp"

#include <exception>
#include <utility>

namespace skai {
namespace {

std::size_t index(HealthComponent component) {
    return static_cast<std::size_t>(component);
}

} // namespace

const char* health_state_name(HealthState state) {
    switch (state) {
    case HealthState::Starting: return "STARTING";
    case HealthState::Running: return "RUNNING";
    case HealthState::Degraded: return "DEGRADED";
    case HealthState::Stopping: return "STOPPING";
    case HealthState::Failed: return "FAILED";
    }
    return "FAILED";
}

const char* health_component_name(HealthComponent component) {
    switch (component) {
    case HealthComponent::VideoSource: return "video_source";
    case HealthComponent::Detector: return "detector";
    case HealthComponent::Encoder: return "encoder";
    case HealthComponent::Recorder: return "recorder";
    case HealthComponent::Gps: return "gps";
    case HealthComponent::Web: return "web";
    case HealthComponent::WebRtc: return "webrtc";
    case HealthComponent::Database: return "database";
    }
    return "unknown";
}

ComponentHealth video_health(const std::optional<RtspDiagnostics>& diagnostics,
                             int stall_timeout_ms) {
    if (!diagnostics) return {HealthState::Starting, "source not initialized"};
    const auto& value = *diagnostics;
    if (value.health == SourceHealth::Connected && value.last_frame_age_ms >= 0 &&
        value.last_frame_age_ms > stall_timeout_ms) {
        return {HealthState::Degraded, "source frame is stale"};
    }
    switch (value.health) {
    case SourceHealth::Connected: return {HealthState::Running, {}};
    case SourceHealth::Connecting:
        return {value.frames_received == 0 ? HealthState::Starting : HealthState::Degraded,
                "source connecting"};
    case SourceHealth::Degraded: return {HealthState::Degraded, "source packet loss"};
    case SourceHealth::Stalled: return {HealthState::Degraded, "source stalled"};
    case SourceHealth::Reconnecting:
        return {HealthState::Degraded, value.last_error.empty() ? "source reconnecting" : value.last_error};
    case SourceHealth::Error:
        return {HealthState::Failed, value.last_error.empty() ? "source failed" : value.last_error};
    case SourceHealth::Stopped: return {HealthState::Failed, "source stopped"};
    }
    return {HealthState::Failed, "unknown source state"};
}

ComponentHealth detector_health(bool supported, bool enabled,
                                const RuntimeStatusSnapshot& status) {
    if (!supported) return {HealthState::Running, "not installed"};
    if (!enabled) return {HealthState::Running, "disabled"};
    if (!status.last_inference_ms) return {HealthState::Degraded, "no current inference"};
    if (status.last_inference_age_ms && *status.last_inference_age_ms > 3000) {
        return {HealthState::Degraded, "inference is stale"};
    }
    return {HealthState::Running, {}};
}

ComponentHealth encoder_health(const RuntimeStatusSnapshot& status, bool media_available,
                               bool h264_required) {
    if (!h264_required && !status.encoder) return {HealthState::Running, "not required"};
    if (!media_available) return {HealthState::Degraded, "H.264 media unavailable"};
    if (!status.encoder) return {HealthState::Running, "H.264 passthrough"};
    if (!status.encoder->last_error.empty()) return {HealthState::Degraded, status.encoder->last_error};
    if (status.encoder->last_access_unit_age_ms > 3000) {
        return {HealthState::Degraded, "encoded media is stale"};
    }
    return {HealthState::Running, {}};
}

ComponentHealth recorder_health(const RecordingStatus& status, bool requested,
                                bool enabled_by_config) {
    if (status.state == "error") return {HealthState::Failed, status.last_error};
    if (!status.configured || (!enabled_by_config && !requested && !status.active)) {
        return {HealthState::Running, "disabled"};
    }
    if (status.state == "unavailable") {
        return {HealthState::Degraded, status.unavailable_reason};
    }
    if (!requested) return {HealthState::Running, "stopped"};
    if (!status.available) return {HealthState::Degraded, status.unavailable_reason};
    if (status.active) return {HealthState::Running, {}};
    return {HealthState::Starting, "recording starting"};
}

ComponentHealth gps_health(bool enabled, const std::optional<GpsFix>& fix) {
    if (!enabled) return {HealthState::Running, "disabled"};
    if (!fix || !fix->valid) return {HealthState::Degraded, "GPS fix unavailable"};
    return {HealthState::Running, {}};
}

ComponentHealth webrtc_health(const WebRtcDiagnostics& diagnostics, const WhipMetrics& whip) {
    if (whip.enabled) {
        if (whip.peer_state == "failed" || whip.peer_state == "stopped") {
            return {HealthState::Failed, whip.last_error.empty() ? "WHIP uplink stopped" : whip.last_error};
        }
        if (whip.peer_state == "retrying" || whip.peer_state == "disconnected" ||
            whip.ice_state == "failed" || whip.ice_state == "disconnected") {
            return {HealthState::Degraded, whip.last_error.empty() ? "WHIP uplink reconnecting" : whip.last_error};
        }
        if (whip.peer_state != "connected" ||
            (whip.ice_state != "connected" && whip.ice_state != "completed")) {
            return {HealthState::Starting, "WHIP uplink connecting"};
        }
    }
    if (diagnostics.enabled && !diagnostics.media_available) {
        return {HealthState::Degraded, diagnostics.media_unavailable_reason};
    }
    if (!diagnostics.enabled && !whip.enabled) return {HealthState::Running, "disabled"};
    return {HealthState::Running, {}};
}

ComponentHealth web_health(bool serving) {
    return serving ? ComponentHealth{HealthState::Running, {}}
                   : ComponentHealth{HealthState::Failed, "HTTP server not serving"};
}

ComponentHealth database_health(const std::string& error) {
    return error.empty() ? ComponentHealth{HealthState::Running, {}}
                         : ComponentHealth{HealthState::Failed, error};
}

HealthWatchdog::~HealthWatchdog() { stop(); }

void HealthWatchdog::set_probe(HealthComponent component, Probe probe, bool critical) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = entries_[index(component)];
    entry.probe = std::move(probe);
    entry.critical = critical;
}

void HealthWatchdog::set_lifecycle(HealthState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    lifecycle_ = state;
}

void HealthWatchdog::tick(Clock::time_point now) {
    std::array<ComponentHealth, 8> observed;
    for (const auto component : all_health_components) {
        Probe probe;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            probe = entries_[index(component)].probe;
        }
        if (!probe) {
            observed[index(component)] = {HealthState::Starting, "probe not registered"};
            continue;
        }
        try {
            observed[index(component)] = probe();
        } catch (const std::exception& error) {
            observed[index(component)] = {HealthState::Failed, error.what()};
        } catch (...) {
            observed[index(component)] = {HealthState::Failed, "probe failed"};
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto component : all_health_components) {
        entries_[index(component)].health = std::move(observed[index(component)]);
    }
    last_poll_ = now;
}

HealthSnapshot HealthWatchdog::snapshot(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    HealthSnapshot result;
    const bool stale = last_poll_ != Clock::time_point{} && now - last_poll_ > std::chrono::seconds(2);
    bool starting = false;
    bool degraded = false;
    bool failed = stale;
    for (const auto component : all_health_components) {
        const auto& entry = entries_[index(component)];
        auto& output = result.components[index(component)];
        output = stale ? ComponentHealth{HealthState::Failed, "watchdog poll stale"} : entry.health;
        if (output.state == HealthState::Failed) {
            if (entry.critical) failed = true;
            else degraded = true;
        } else if (output.state == HealthState::Degraded) degraded = true;
        else if (output.state == HealthState::Starting) starting = true;
    }
    result.state = lifecycle_ == HealthState::Running
        ? failed ? HealthState::Failed : degraded ? HealthState::Degraded
          : starting ? HealthState::Starting : HealthState::Running
        : lifecycle_;
    return result;
}

void HealthWatchdog::start() {
    if (worker_.joinable()) return;
    tick();
    set_lifecycle(HealthState::Running);
    worker_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (wake_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_; })) break;
            lock.unlock();
            tick();
            lock.lock();
        }
    });
}

void HealthWatchdog::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        lifecycle_ = HealthState::Stopping;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

} // namespace skai
