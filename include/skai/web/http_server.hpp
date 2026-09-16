#pragma once

#include "skai/application.hpp"
#include "skai/api_state.hpp"
#include "skai/events.hpp"
#include "skai/logging.hpp"
#include "skai/status.hpp"

#include <memory>

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
    ~HttpServer() override;

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

    unsigned short port() const noexcept;

private:
    struct State;
    Logger& logger_;
    std::shared_ptr<RuntimeStatus> status_;
    std::shared_ptr<ApiState> api_;
    std::shared_ptr<EventChannel> events_;
    std::unique_ptr<State> state_;
};

} // namespace skai::web
