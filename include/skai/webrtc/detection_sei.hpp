#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skai {

// SEI contract v1 (ROADMAP Step 28): detections ride to cloud viewers inside
// the WHIP H.264 stream as user_data_unregistered SEI with this UUID.
inline constexpr std::array<std::uint8_t, 16> kDetectionSeiUuid = {
    0x19, 0x7c, 0x65, 0xee, 0xa1, 0x32, 0x4f, 0x53,
    0x85, 0xa0, 0x8a, 0xf9, 0xad, 0x15, 0x3c, 0x4b};

// One box in normalized top-left/size form, each value rounded to 4 decimals.
struct SeiBox {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
    std::string class_name;
    double score = 0.0;
};

// One inference result as sent in the SEI `r` array.
struct SeiResult {
    std::uint32_t dt = 0;
    std::vector<SeiBox> boxes;
};

// Converts a pixel-space corner box to the normalized SEI form, clamped to the frame.
SeiBox normalize_sei_box(float x1, float y1, float x2, float y2, int frame_width,
                         int frame_height, std::string class_name, float score);

// dt = round((attach - source) * 90000 / 1e9); empty when source is newer.
std::optional<std::uint32_t> sei_dt_ticks(std::uint64_t attach_pts_ns,
                                          std::uint64_t source_pts_ns);

// {"v":1,"r":[{"dt":...,"b":[[x,y,w,h,"class",score],...]},...]}
std::string detection_sei_json(const std::vector<SeiResult>& results);

// Annex-B SEI NAL (4-byte start code) carrying UUID + payload, with emulation
// prevention applied to the whole RBSP including the trailing bits.
std::vector<std::uint8_t> encode_user_data_sei_nal(
    const std::array<std::uint8_t, 16>& uuid, std::string_view payload);

// Inserts `nal` before the first VCL NAL (types 1-5) of an Annex-B access unit.
// Returns false and leaves the unit unchanged when it contains no VCL NAL.
bool insert_before_first_vcl(std::vector<std::uint8_t>& access_unit,
                             const std::vector<std::uint8_t>& nal);

// One inference result waiting for a WHIP access unit. `source_generation`
// identifies the RTSP pipeline that produced the inferred frame.
struct SeiSourceResult {
    std::uint64_t source_pts_ns = 0;
    std::uint64_t source_generation = 0;
    std::vector<SeiBox> boxes;
};

struct SeiUplinkStats {
    std::uint64_t sei_units = 0;
    std::uint64_t results_attached = 0;
    std::uint64_t results_dropped = 0;
    std::uint64_t boxes_dropped = 0;
};

inline constexpr std::size_t kMaxDetectionSeiBytes = 1024;

// Applies the Step 28 emission rules to results waiting for the next access
// unit WHIP sends: 1 s window, no results from an earlier RTSP pipeline or
// newer than the unit, oldest first, and at most kMaxDetectionSeiBytes.
class SeiResultSelector {
public:
    explicit SeiResultSelector(std::size_t max_pending = 64) : max_pending_(max_pending) {}

    void add(SeiSourceResult result);
    // The SEI NAL for the unit about to be sent, or empty when none is due.
    std::vector<std::uint8_t> take_for(std::uint64_t attach_pts_ns,
                                       std::uint64_t attach_generation);
    std::size_t pending() const { return pending_.size(); }
    const SeiUplinkStats& stats() const { return stats_; }
    // Nearest-rank percentile of recently attached dt values, q in [0, 1].
    std::optional<std::uint32_t> dt_percentile(double q) const;

private:
    std::size_t max_pending_;
    std::vector<SeiSourceResult> pending_;
    std::vector<std::uint32_t> recent_dt_;
    std::size_t next_dt_ = 0;
    SeiUplinkStats stats_;
};

} // namespace skai
