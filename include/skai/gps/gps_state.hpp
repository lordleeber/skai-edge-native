#pragma once

#include "skai/gps/gps_source.hpp"

#include <mutex>
#include <optional>

namespace skai {

class GpsState {
public:
    void update(std::optional<GpsFix> fix);
    void clear() noexcept;
    std::optional<GpsFix> latest() const;

private:
    mutable std::mutex mutex_;
    std::optional<GpsFix> latest_;
};

} // namespace skai
