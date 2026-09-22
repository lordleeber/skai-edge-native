#pragma once

#include <cstdint>
#include <vector>

namespace skai {

// One complete H.264 access unit in Annex-B (byte-stream) format.
struct EncodedAccessUnit {
    std::uint64_t sequence = 0;
    std::uint64_t pts_ns = 0;
    bool keyframe = false;
    std::vector<std::uint8_t> bytes;
};

} // namespace skai
