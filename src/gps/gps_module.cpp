#include "skai/gps/gps_module.hpp"

#include <utility>

namespace skai {

GpsModule::GpsModule(std::shared_ptr<GpsState> state)
    : state_(state ? std::move(state) : std::make_shared<GpsState>()) {}

bool GpsModule::initialize(const Config& config) {
    if (source_) return false;
    source_ = std::make_unique<GpsSource>(config.gps);
    state_->update(source_->latest());
    return true;
}

bool GpsModule::start() {
    return source_ != nullptr;
}

void GpsModule::stop() noexcept {}

void GpsModule::wait() noexcept {
    state_->clear();
    source_.reset();
}

} // namespace skai
