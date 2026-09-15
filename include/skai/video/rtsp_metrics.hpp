#pragma once

#include <cstdint>

namespace skai {

// Jitterbuffer stats are cumulative. Ignore temporary backwards readings;
// reset only when a new jitterbuffer starts its own counter epoch.
class MonotonicCounter {
public:
    std::uint64_t observe(std::uint64_t value) {
        if (value <= high_water_) return 0;
        const auto increase = value - high_water_;
        high_water_ = value;
        return increase;
    }

    void reset() { high_water_ = 0; }

private:
    std::uint64_t high_water_ = 0;
};

} // namespace skai
