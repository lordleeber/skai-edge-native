#pragma once

#include "skai/application.hpp"
#include "skai/api_state.hpp"
#include "skai/events.hpp"
#include "skai/health.hpp"
#include "skai/logging.hpp"
#include "skai/metrics.hpp"
#include "skai/status.hpp"
#include "skai/storage/alert_repository.hpp"
#include "skai/video/recording_control.hpp"
#include "skai/webrtc/webrtc_manager.hpp"

#include <atomic>
#include <memory>
#include <functional>

namespace skai::web {

class HttpServer final : public LifecycleModule {
public:
    explicit HttpServer(Logger& logger);
    HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status);
    HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status,
               std::shared_ptr<ApiState> api);
    HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status,
               std::shared_ptr<ApiState> api,
               std::shared_ptr<EventChannel> events);
    HttpServer(Logger& logger, std::shared_ptr<RuntimeStatus> status,
               std::shared_ptr<ApiState> api,
               std::shared_ptr<EventChannel> events,
               std::shared_ptr<AlertRepository> alerts,
               std::shared_ptr<RecordingController> recording = {},
               std::shared_ptr<WebRtcManager> webrtc = {},
               std::function<MetricsSnapshot()> metrics_provider = {},
               std::function<HealthSnapshot()> health_provider = {});
    ~HttpServer() override;

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

    unsigned short port() const noexcept;
    bool serving() const noexcept { return serving_.load(); }

private:
    struct State;
    Logger& logger_;
    std::shared_ptr<RuntimeStatus> status_;
    std::shared_ptr<ApiState> api_;
    std::shared_ptr<EventChannel> events_;
    std::shared_ptr<AlertRepository> alerts_;
    std::shared_ptr<RecordingController> recording_;
    std::shared_ptr<WebRtcManager> webrtc_;
    std::function<MetricsSnapshot()> metrics_provider_;
    std::function<HealthSnapshot()> health_provider_;
    std::atomic<bool> serving_{false};
    std::unique_ptr<State> state_;
};

} // namespace skai::web
