#pragma once

#include "skai/application.hpp"
#include "skai/logging.hpp"

namespace skai {

// Configures skai-ice process-wide settings before any PeerConnection exists.
class IceRuntimeModule final : public LifecycleModule {
public:
    explicit IceRuntimeModule(Logger& logger) : logger_(logger) {}

    bool initialize(const Config& config) override;
    bool start() override { return true; }
    void stop() noexcept override {}
    void wait() noexcept override {}

private:
    Logger& logger_;
};

} // namespace skai
