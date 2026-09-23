#pragma once

#include <cstdint>
#include <vector>

namespace skai {

// One complete H.264 access unit in Annex-B (byte-stream) format.
struct EncodedAccessUnit {
    std::uint64_t sequence = 0;
    std::uint64_t pts_ns = 0;
    bool keyframe = false;
    // True for the first AU after the RTSP media pipeline is (re)created.
    bool discontinuity = false;
    std::vector<std::uint8_t> bytes;
};

} // namespace skai
