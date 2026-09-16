#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace skai {

// Packed, CPU-addressable BGR pixels; stride may include row padding.
struct Frame {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp;
    std::uint64_t pts_ns = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> bgr;
};

} // namespace skai
