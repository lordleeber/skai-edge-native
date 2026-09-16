#pragma once

#include "skai/application.hpp"
#include "skai/logging.hpp"

#include <memory>

namespace skai::web {

class HttpServer final : public LifecycleModule {
public:
    explicit HttpServer(Logger& logger);
    ~HttpServer() override;

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

    unsigned short port() const noexcept;

private:
    struct State;
    Logger& logger_;
    std::unique_ptr<State> state_;
};

} // namespace skai::web
