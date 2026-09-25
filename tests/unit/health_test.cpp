#include "skai/health.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

using namespace std::chrono_literals;

TEST(HealthWatchdog, ReportsEveryComponentAndAggregatesFailures) {
    skai::HealthWatchdog watchdog;
    const auto now = skai::HealthWatchdog::Clock::now();
    for (const auto component : skai::all_health_components) {
        watchdog.set_probe(component, [] { return skai::ComponentHealth{skai::HealthState::Running, {}}; },
                           component == skai::HealthComponent::Database ||
                           component == skai::HealthComponent::Web);
    }
    EXPECT_EQ(watchdog.snapshot(now).state, skai::HealthState::Starting);
    watchdog.set_lifecycle(skai::HealthState::Running);
    watchdog.tick(now);
    EXPECT_EQ(watchdog.snapshot(now).state, skai::HealthState::Running);

    watchdog.set_probe(skai::HealthComponent::Recorder, [] {
        return skai::ComponentHealth{skai::HealthState::Failed, "disk full"};
    });
    watchdog.tick(now + 1s);
    EXPECT_EQ(watchdog.snapshot(now + 1s).state, skai::HealthState::Degraded);
    EXPECT_EQ(watchdog.snapshot(now + 1s).components[3].detail, "disk full");

    watchdog.set_probe(skai::HealthComponent::Database, [] {
        return skai::ComponentHealth{skai::HealthState::Failed, "database closed"};
    }, true);
    watchdog.tick(now + 2s);
    EXPECT_EQ(watchdog.snapshot(now + 2s).state, skai::HealthState::Failed);
    watchdog.set_lifecycle(skai::HealthState::Stopping);
    EXPECT_EQ(watchdog.snapshot(now + 2s).state, skai::HealthState::Stopping);
}

TEST(HealthWatchdog, DetectsStaleProbeAndIsolatesExceptions) {
    skai::HealthWatchdog watchdog;
    const auto now = skai::HealthWatchdog::Clock::now();
    for (const auto component : skai::all_health_components) {
        watchdog.set_probe(component, [] { return skai::ComponentHealth{skai::HealthState::Running, {}}; });
    }
    watchdog.set_lifecycle(skai::HealthState::Running);
    watchdog.tick(now);
    EXPECT_EQ(watchdog.snapshot(now + 3s).state, skai::HealthState::Failed);
    watchdog.set_probe(skai::HealthComponent::Gps, []() -> skai::ComponentHealth {
        throw std::runtime_error("GPS probe failed");
    });
    watchdog.tick(now + 4s);
    const auto result = watchdog.snapshot(now + 4s);
    EXPECT_EQ(result.state, skai::HealthState::Degraded);
    EXPECT_EQ(result.components[4].detail, "GPS probe failed");
}

TEST(HealthWatchdog, StartsPollingAndTransitionsToStopping) {
    skai::HealthWatchdog watchdog;
    for (const auto component : skai::all_health_components) {
        watchdog.set_probe(component, [] { return skai::ComponentHealth{skai::HealthState::Running, {}}; });
    }
    watchdog.start();
    EXPECT_EQ(watchdog.snapshot().state, skai::HealthState::Running);
    watchdog.stop();
    EXPECT_EQ(watchdog.snapshot().state, skai::HealthState::Stopping);
}

TEST(HealthModel, ClassifiesSourceAndPeerIndependently) {
    skai::RtspDiagnostics source;
    source.health = skai::SourceHealth::Reconnecting;
    source.last_error = "source offline";
    EXPECT_EQ(skai::video_health(source).state, skai::HealthState::Degraded);
    source.health = skai::SourceHealth::Connected;
    source.last_frame_age_ms = 4000;
    EXPECT_EQ(skai::video_health(source).state, skai::HealthState::Degraded);
    source.last_frame_age_ms = 100;
    EXPECT_EQ(skai::video_health(source).state, skai::HealthState::Running);

    skai::WebRtcDiagnostics webrtc;
    webrtc.enabled = true;
    webrtc.media_available = true;
    webrtc.recently_closed.emplace_back();
    webrtc.recently_closed.back().last_error = "peer failed";
    EXPECT_EQ(skai::webrtc_health(webrtc).state, skai::HealthState::Running);
    webrtc.media_available = false;
    EXPECT_EQ(skai::webrtc_health(webrtc).state, skai::HealthState::Degraded);
}

TEST(HealthModel, RecorderFailureSurvivesMediaLossAndDisabledRecorderIsHealthy) {
    skai::RecordingStatus recording;
    recording.configured = true;
    recording.state = "error";
    recording.last_error = "disk full";
    EXPECT_EQ(skai::recorder_health(recording, false, true).state, skai::HealthState::Failed);
    recording.available = false;
    EXPECT_EQ(skai::recorder_health(recording, false, true).detail, "disk full");
    recording.state = "unavailable";
    recording.unavailable_reason = "RTSP reconnecting";
    EXPECT_EQ(skai::recorder_health(recording, false, true).state, skai::HealthState::Degraded);
    EXPECT_EQ(skai::recorder_health(recording, false, false).state, skai::HealthState::Running);
}

TEST(HealthModel, DetectorWatchdogRejectsStaleInference) {
    skai::RuntimeStatusSnapshot status;
    status.last_inference_ms = 12.0;
    status.last_inference_age_ms = 4000;
    EXPECT_EQ(skai::detector_health(true, true, status).state, skai::HealthState::Degraded);
    status.last_inference_age_ms = 100;
    EXPECT_EQ(skai::detector_health(true, true, status).state, skai::HealthState::Running);
    EXPECT_EQ(skai::detector_health(true, false, status).state, skai::HealthState::Running);
}
