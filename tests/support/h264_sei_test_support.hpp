#pragma once

// Viewer-side H.264 SEI reading and a small x264 test stream, shared by the
// SEI unit tests and the WHIP end-to-end test.

#include "skai/video/gstreamer_runtime.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace skai_test {

using Bytes = std::vector<std::uint8_t>;

// Viewer-side reading of the SEI contract, written from H.264 7.3.2.3 / D.1.6
// rather than from the encoder, so both sides must agree on the byte layout.
struct Nal {
    std::uint8_t type = 0;
    Bytes ebsp; // bytes after the one-byte NAL header
};

inline std::vector<Nal> split_annex_b(const Bytes& bytes) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            starts.push_back(i + 3);
            i += 2;
        }
    }
    std::vector<Nal> nals;
    for (std::size_t n = 0; n < starts.size(); ++n) {
        auto end = n + 1 < starts.size() ? starts[n + 1] - 3 : bytes.size();
        while (end > starts[n] && bytes[end - 1] == 0) --end;
        if (end <= starts[n]) continue;
        nals.push_back({static_cast<std::uint8_t>(bytes[starts[n]] & 0x1f),
                        Bytes(bytes.begin() + starts[n] + 1, bytes.begin() + end)});
    }
    return nals;
}

inline Bytes remove_emulation_prevention(const Bytes& ebsp) {
    Bytes rbsp;
    int zeros = 0;
    for (const auto byte : ebsp) {
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return rbsp;
}

struct ParsedSei {
    std::array<std::uint8_t, 16> uuid{};
    std::string payload;
    bool trailing_bits_ok = false;
};

inline std::optional<ParsedSei> parse_user_data_sei(const Nal& nal) {
    if (nal.type != 6) return std::nullopt;
    const auto rbsp = remove_emulation_prevention(nal.ebsp);
    std::size_t pos = 0;
    std::size_t type = 0;
    while (pos < rbsp.size() && rbsp[pos] == 0xff) type += rbsp[pos++];
    if (pos >= rbsp.size()) return std::nullopt;
    type += rbsp[pos++];
    std::size_t size = 0;
    while (pos < rbsp.size() && rbsp[pos] == 0xff) size += rbsp[pos++];
    if (pos >= rbsp.size()) return std::nullopt;
    size += rbsp[pos++];
    if (type != 5 || size < 16 || pos + size > rbsp.size()) return std::nullopt;
    ParsedSei sei;
    std::copy(rbsp.begin() + pos, rbsp.begin() + pos + 16, sei.uuid.begin());
    sei.payload.assign(rbsp.begin() + pos + 16, rbsp.begin() + pos + size);
    pos += size;
    sei.trailing_bits_ok = pos + 1 == rbsp.size() && rbsp[pos] == 0x80;
    return sei;
}

struct EncodedUnit {
    Bytes bytes;
    std::uint64_t pts_ns = 0;
};

inline std::vector<EncodedUnit> encode_test_stream(int frames) {
    std::string error;
    EXPECT_TRUE(skai::gst::initialize_once(error)) << error;
    const auto launch =
        "videotestsrc num-buffers=" + std::to_string(frames) +
        " ! video/x-raw,width=320,height=240,framerate=25/1 ! videoconvert"
        " ! x264enc key-int-max=10 bframes=0 aud=true tune=zerolatency"
        " ! video/x-h264,stream-format=byte-stream,alignment=au ! appsink name=out sync=false";
    GError* parse_error = nullptr;
    GstElement* pipeline = gst_parse_launch(launch.c_str(), &parse_error);
    if (parse_error) {
        ADD_FAILURE() << parse_error->message;
        g_error_free(parse_error);
    }
    std::vector<EncodedUnit> units;
    if (!pipeline) return units;
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    while (GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND)) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            units.push_back({Bytes(map.data, map.data + map.size),
                             static_cast<std::uint64_t>(GST_BUFFER_PTS(buffer))});
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return units;
}

struct ParsedBox {
    std::string class_name;
    double score = 0.0;
};

struct ParsedResult {
    std::uint32_t dt = 0;
    std::vector<ParsedBox> boxes;
};

// Splits the contract JSON into results; relies only on the documented shape.
inline std::vector<ParsedResult> parse_results(const std::string& json) {
    std::vector<ParsedResult> results;
    const std::regex result("\\{\"dt\":([0-9]+),\"b\":\\[(.*?)\\]\\}");
    const std::regex box("\\[[-0-9.]+,[-0-9.]+,[-0-9.]+,[-0-9.]+,\"([^\"]*)\",([-0-9.]+)\\]");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), result);
         it != std::sregex_iterator(); ++it) {
        ParsedResult parsed;
        parsed.dt = static_cast<std::uint32_t>(std::stoul((*it)[1]));
        const std::string boxes = (*it)[2];
        for (auto b = std::sregex_iterator(boxes.begin(), boxes.end(), box);
             b != std::sregex_iterator(); ++b) {
            parsed.boxes.push_back({(*b)[1], std::stod((*b)[2])});
        }
        results.push_back(std::move(parsed));
    }
    return results;
}

} // namespace skai_test
