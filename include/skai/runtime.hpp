#pragma once

#include "skai/application.hpp"
#include "skai/inference/inference_backend.hpp"
#include "skai/events.hpp"
#include "skai/status.hpp"

namespace skai {
class ApiState;
class RecordingController;
class WebRtcManager;
class AlertRepository;

// Owns the production module graph, queues and health watchdog. The only
// replaceable dependency is inference; modules and their wiring are shared.
class Runtime {
public:
    Runtime(std::string config_path, Logger& logger,
            std::shared_ptr<EventChannel> events = {}, InferenceBackendFactory backend_factory = {});
    ~Runtime();
    bool initialize();
    bool start();
    void stop() noexcept;
    void wait() noexcept;
    Config config() const;
    std::string last_error() const;
    std::string last_error_module() const;
    unsigned short http_port() const;
    std::shared_ptr<RuntimeStatus> status() const;
    std::shared_ptr<ApiState> api_state() const;
    std::shared_ptr<RecordingController> recording_control() const;
    std::shared_ptr<WebRtcManager> webrtc_manager() const;
    std::shared_ptr<AlertRepository> alert_repository() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

std::unique_ptr<Runtime> create_runtime(std::string config_path, Logger& logger,
    std::shared_ptr<EventChannel> events = {}, InferenceBackendFactory backend_factory = {});
} // namespace skai
