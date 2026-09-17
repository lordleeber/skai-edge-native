#pragma once

#include "skai/application.hpp"
#include "skai/gps/gps_source.hpp"
#include "skai/gps/gps_state.hpp"

#include <memory>

namespace skai {

class GpsModule final : public LifecycleModule {
public:
    explicit GpsModule(std::shared_ptr<GpsState> state);

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

private:
    std::shared_ptr<GpsState> state_;
    std::unique_ptr<GpsSource> source_;
};

} // namespace skai
