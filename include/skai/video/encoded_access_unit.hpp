#pragma once

#include <cstdint>
#include <vector>

namespace skai {

// One complete H.264 access unit in Annex-B (byte-stream) format.
struct EncodedAccessUnit {
    std::uint64_t sequence = 0;
    std::uint64_t pts_ns = 0;
    std::uint64_t dts_ns = 0;
    bool has_pts = false;
    bool has_dts = false;
    bool keyframe = false;
    // True for the first AU after an RTSP or encoder pipeline is (re)created.
    bool discontinuity = false;
    // Identifies the RTSP media pipeline instance; PTS restarts when it changes.
    std::uint64_t source_generation = 0;
    std::vector<std::uint8_t> bytes;
};

inline std::uint32_t h264_rtp_timestamp(std::uint64_t pts_ns) noexcept {
    const std::uint64_t seconds = pts_ns / 1'000'000'000ULL;
    const std::uint64_t remainder = pts_ns % 1'000'000'000ULL;
    return static_cast<std::uint32_t>(seconds * 90'000ULL +
                                     remainder * 90'000ULL / 1'000'000'000ULL);
}

} // namespace skai
