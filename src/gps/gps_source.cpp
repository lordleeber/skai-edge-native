#include "skai/gps/gps_source.hpp"

namespace skai {

GpsSource::GpsSource(const GpsConfig& config) : config_(config) {}

std::optional<GpsFix> GpsSource::latest() const {
    if (!config_.enabled || config_.source != "fixed") return std::nullopt;
    return GpsFix{true, "fixed", config_.latitude, config_.longitude,
                  config_.altitude_m, 0.9, 12, 10};
}

} // namespace skai
