#include "skai/webrtc/detection_sei.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace skai {
namespace {

double round4(double value) { return std::round(value * 10000.0) / 10000.0; }

double clamp_unit(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

// Locale-independent, shortest form of a value already rounded to 4 decimals.
void append_number(std::string& out, double value) {
    if (!std::isfinite(value) || std::abs(value) > 1e9) value = 0.0;
    auto scaled = static_cast<long long>(std::llround(value * 10000.0));
    if (scaled < 0) {
        out += '-';
        scaled = -scaled;
    }
    out += std::to_string(scaled / 10000);
    auto fraction = scaled % 10000;
    if (fraction == 0) return;
    int digits = 4;
    while (fraction % 10 == 0) {
        fraction /= 10;
        --digits;
    }
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), ".%0*lld", digits, fraction);
    out += buffer;
}

void append_string(std::string& out, const std::string& value) {
    out += '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

bool is_vcl(std::uint8_t header) {
    const auto type = header & 0x1f;
    return type >= 1 && type <= 5;
}

} // namespace

SeiBox normalize_sei_box(float x1, float y1, float x2, float y2, int frame_width,
                         int frame_height, std::string class_name, float score) {
    SeiBox box;
    box.class_name = std::move(class_name);
    box.score = round4(clamp_unit(score));
    if (frame_width <= 0 || frame_height <= 0) return box;
    const double left = clamp_unit(static_cast<double>(x1) / frame_width);
    const double top = clamp_unit(static_cast<double>(y1) / frame_height);
    const double right = clamp_unit(static_cast<double>(x2) / frame_width);
    const double bottom = clamp_unit(static_cast<double>(y2) / frame_height);
    box.x = round4(left);
    box.y = round4(top);
    box.w = round4(std::max(0.0, right - left));
    box.h = round4(std::max(0.0, bottom - top));
    return box;
}

std::optional<std::uint32_t> sei_dt_ticks(std::uint64_t attach_pts_ns,
                                          std::uint64_t source_pts_ns) {
    if (source_pts_ns > attach_pts_ns) return std::nullopt;
    const auto elapsed = attach_pts_ns - source_pts_ns;
    // round(elapsed * 90000 / 1e9) == round(elapsed * 9 / 100000)
    return static_cast<std::uint32_t>((elapsed * 9 + 50'000) / 100'000);
}

std::string detection_sei_json(const std::vector<SeiResult>& results) {
    std::string out = "{\"v\":1,\"r\":[";
    for (std::size_t r = 0; r < results.size(); ++r) {
        if (r) out += ',';
        out += "{\"dt\":" + std::to_string(results[r].dt) + ",\"b\":[";
        const auto& boxes = results[r].boxes;
        for (std::size_t b = 0; b < boxes.size(); ++b) {
            if (b) out += ',';
            out += '[';
            append_number(out, boxes[b].x);
            out += ',';
            append_number(out, boxes[b].y);
            out += ',';
            append_number(out, boxes[b].w);
            out += ',';
            append_number(out, boxes[b].h);
            out += ',';
            append_string(out, boxes[b].class_name);
            out += ',';
            append_number(out, boxes[b].score);
            out += ']';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

std::vector<std::uint8_t> encode_user_data_sei_nal(
    const std::array<std::uint8_t, 16>& uuid, std::string_view payload) {
    std::vector<std::uint8_t> rbsp;
    rbsp.push_back(5); // payloadType: user_data_unregistered
    auto size = uuid.size() + payload.size();
    while (size >= 255) {
        rbsp.push_back(0xff);
        size -= 255;
    }
    rbsp.push_back(static_cast<std::uint8_t>(size));
    rbsp.insert(rbsp.end(), uuid.begin(), uuid.end());
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
    rbsp.push_back(0x80); // rbsp_trailing_bits

    std::vector<std::uint8_t> nal = {0, 0, 0, 1, 0x06};
    nal.reserve(nal.size() + rbsp.size() + rbsp.size() / 64 + 1);
    int zeros = 0;
    for (const auto byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            nal.push_back(3); // emulation_prevention_three_byte
            zeros = 0;
        }
        nal.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return nal;
}

bool insert_before_first_vcl(std::vector<std::uint8_t>& access_unit,
                             const std::vector<std::uint8_t>& nal) {
    for (std::size_t i = 0; i + 3 < access_unit.size(); ++i) {
        if (access_unit[i] != 0 || access_unit[i + 1] != 0 || access_unit[i + 2] != 1) {
            continue;
        }
        if (is_vcl(access_unit[i + 3])) {
            // Keep a 4-byte start code's zero_byte with the slice it introduces.
            const auto at = i > 0 && access_unit[i - 1] == 0 ? i - 1 : i;
            access_unit.insert(access_unit.begin() + static_cast<std::ptrdiff_t>(at),
                               nal.begin(), nal.end());
            return true;
        }
        i += 2;
    }
    return false;
}

} // namespace skai

namespace skai {

namespace {

constexpr std::uint64_t kResultWindowNs = 1'000'000'000ULL;
constexpr std::size_t kRecentDtSamples = 4096;

std::vector<std::uint8_t> encode_results(const std::vector<SeiResult>& results) {
    return encode_user_data_sei_nal(kDetectionSeiUuid, detection_sei_json(results));
}

} // namespace

void SeiResultSelector::add(SeiSourceResult result) {
    pending_.push_back(std::move(result));
    if (pending_.size() > max_pending_) {
        const auto oldest = std::min_element(
            pending_.begin(), pending_.end(), [](const auto& a, const auto& b) {
                return std::tie(a.source_generation, a.source_pts_ns) <
                       std::tie(b.source_generation, b.source_pts_ns);
            });
        pending_.erase(oldest);
        ++stats_.results_dropped;
    }
}

std::vector<std::uint8_t> SeiResultSelector::take_for(std::uint64_t attach_pts_ns,
                                                      std::uint64_t attach_generation) {
    std::vector<SeiSourceResult> due;
    std::vector<SeiSourceResult> later;
    for (auto& result : pending_) {
        if (result.source_generation > attach_generation) {
            later.push_back(std::move(result)); // its pipeline's units are not sent yet
        } else if (result.source_generation < attach_generation ||
                   result.source_pts_ns > attach_pts_ns ||
                   attach_pts_ns - result.source_pts_ns > kResultWindowNs) {
            ++stats_.results_dropped;
        } else {
            due.push_back(std::move(result));
        }
    }
    pending_ = std::move(later);
    if (due.empty()) return {};
    std::stable_sort(due.begin(), due.end(), [](const auto& a, const auto& b) {
        return a.source_pts_ns < b.source_pts_ns;
    });

    std::vector<SeiResult> results;
    results.reserve(due.size());
    for (auto& result : due) {
        results.push_back({*sei_dt_ticks(attach_pts_ns, result.source_pts_ns),
                           std::move(result.boxes)});
    }
    auto nal = encode_results(results);
    while (nal.size() > kMaxDetectionSeiBytes && results.size() > 1) {
        results.erase(results.begin());
        ++stats_.results_dropped;
        nal = encode_results(results);
    }
    if (nal.size() > kMaxDetectionSeiBytes) {
        // The newest result alone is too large: keep its highest-score boxes.
        auto boxes = std::move(results.front().boxes);
        std::stable_sort(boxes.begin(), boxes.end(),
                         [](const auto& a, const auto& b) { return a.score > b.score; });
        std::size_t fits = 0;
        std::size_t too_many = boxes.size();
        while (fits + 1 < too_many) {
            const auto count = (fits + too_many) / 2;
            results.front().boxes.assign(boxes.begin(), boxes.begin() + count);
            if (encode_results(results).size() <= kMaxDetectionSeiBytes) fits = count;
            else too_many = count;
        }
        results.front().boxes.assign(boxes.begin(), boxes.begin() + fits);
        stats_.boxes_dropped += boxes.size() - fits;
        nal = encode_results(results);
    }

    ++stats_.sei_units;
    stats_.results_attached += results.size();
    for (const auto& result : results) {
        if (recent_dt_.size() < kRecentDtSamples) {
            recent_dt_.push_back(result.dt);
        } else {
            recent_dt_[next_dt_] = result.dt;
            next_dt_ = (next_dt_ + 1) % kRecentDtSamples;
        }
    }
    return nal;
}

std::optional<std::uint32_t> SeiResultSelector::dt_percentile(double q) const {
    if (recent_dt_.empty()) return std::nullopt;
    auto sorted = recent_dt_;
    std::sort(sorted.begin(), sorted.end());
    const auto rank = static_cast<std::size_t>(
        std::ceil(std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

} // namespace skai
