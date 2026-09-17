#pragma once

#include "skai/config.hpp"

#include <optional>
#include <string>

namespace skai {

struct GpsFix {
    bool valid = false;
    std::string source;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude_m = 0.0;
    double hdop = 0.0;
    int satellites_visible = 0;
    int satellites_used = 0;
};

class GpsSource {
public:
    explicit GpsSource(const GpsConfig& config);
    std::optional<GpsFix> latest() const;

private:
    GpsConfig config_;
};

} // namespace skai
