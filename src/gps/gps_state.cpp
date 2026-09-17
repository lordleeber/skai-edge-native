#include "skai/gps/gps_state.hpp"

#include <utility>

namespace skai {

void GpsState::update(std::optional<GpsFix> fix) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = std::move(fix);
}

void GpsState::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.reset();
}

std::optional<GpsFix> GpsState::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

} // namespace skai
