#include "skai/video/gstreamer_runtime.hpp"
#include "skai/webrtc/detection_sei.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

// Viewer-side reading of the SEI contract, written from H.264 7.3.2.3 / D.1.6
// rather than from the encoder, so both sides must agree on the byte layout.
struct Nal {
    std::uint8_t type = 0;
    Bytes ebsp; // bytes after the one-byte NAL header
};

std::vector<Nal> split_annex_b(const Bytes& bytes) {
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

Bytes remove_emulation_prevention(const Bytes& ebsp) {
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

std::optional<ParsedSei> parse_user_data_sei(const Nal& nal) {
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

std::vector<std::uint8_t> nal_types(const Bytes& unit) {
    std::vector<std::uint8_t> types;
    for (const auto& nal : split_annex_b(unit)) types.push_back(nal.type);
    return types;
}

std::vector<std::uint32_t> dt_values(const std::string& json) {
    std::vector<std::uint32_t> values;
    const std::regex dt("\"dt\":([0-9]+)");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), dt);
         it != std::sregex_iterator(); ++it) {
        values.push_back(static_cast<std::uint32_t>(std::stoul((*it)[1])));
    }
    return values;
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

} // namespace

TEST(DetectionSei, EncodesUserDataUnregisteredByteLayout) {
    const std::string json = R"({"v":1,"r":[]})";
    const auto nal = skai::encode_user_data_sei_nal(skai::kDetectionSeiUuid, json);
    Bytes expected = {0, 0, 0, 1, 0x06, 0x05,
                      static_cast<std::uint8_t>(16 + json.size())};
    expected.insert(expected.end(), skai::kDetectionSeiUuid.begin(),
                    skai::kDetectionSeiUuid.end());
    expected.insert(expected.end(), json.begin(), json.end());
    expected.push_back(0x80);
    EXPECT_EQ(nal, expected);
}

TEST(DetectionSei, UuidMatchesTheAgreedContract) {
    const std::array<std::uint8_t, 16> agreed = {
        0x19, 0x7c, 0x65, 0xee, 0xa1, 0x32, 0x4f, 0x53,
        0x85, 0xa0, 0x8a, 0xf9, 0xad, 0x15, 0x3c, 0x4b}; // 197c65ee-a132-4f53-85a0-8af9ad153c4b
    EXPECT_EQ(skai::kDetectionSeiUuid, agreed);
}

TEST(DetectionSei, PayloadSizeAbove255UsesFfExtensionBytes) {
    const std::string json(300, 'a');
    const auto nal = skai::encode_user_data_sei_nal(skai::kDetectionSeiUuid, json);
    ASSERT_GT(nal.size(), 8U);
    EXPECT_EQ(nal[6], 0xff);
    EXPECT_EQ(nal[7], 316 - 255);
    const auto nals = split_annex_b(nal);
    ASSERT_EQ(nals.size(), 1U);
    const auto parsed = parse_user_data_sei(nals[0]);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->payload, json);
    EXPECT_TRUE(parsed->trailing_bits_ok);
}

TEST(DetectionSei, EmulationPreventionRoundTripsZeroRuns) {
    const std::array<std::uint8_t, 16> uuid = {0, 0, 0, 0, 0, 1, 0, 0,
                                               2, 0, 0, 3, 0, 0, 0, 0};
    const std::string payload("\0\0\x01\0\0\0\x03\0\0", 9);
    const auto nal = skai::encode_user_data_sei_nal(uuid, payload);
    ASSERT_GT(nal.size(), 5U);
    const Bytes body(nal.begin() + 5, nal.end());
    for (std::size_t i = 0; i + 2 < body.size(); ++i) {
        if (body[i] == 0 && body[i + 1] == 0) {
            EXPECT_GE(body[i + 2], 3) << "start-code emulation at " << i;
        }
    }
    const auto nals = split_annex_b(nal);
    ASSERT_EQ(nals.size(), 1U);
    const auto parsed = parse_user_data_sei(nals[0]);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->uuid, uuid);
    EXPECT_EQ(parsed->payload, payload);
    EXPECT_TRUE(parsed->trailing_bits_ok);
}

TEST(DetectionSei, InsertsAfterAudSpsPpsAndBeforeFirstSlice) {
    const Bytes aud = {0, 0, 0, 1, 0x09, 0xf0};
    const Bytes sps = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f};
    const Bytes pps = {0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
    const Bytes idr = {0, 0, 1, 0x65, 0x88, 0x84};
    const Bytes idr2 = {0, 0, 1, 0x65, 0x11, 0x22};
    auto unit = concat({aud, sps, pps, idr, idr2});
    const auto sei = skai::encode_user_data_sei_nal(skai::kDetectionSeiUuid, "{}");
    ASSERT_TRUE(skai::insert_before_first_vcl(unit, sei));
    EXPECT_EQ(nal_types(unit), (std::vector<std::uint8_t>{9, 7, 8, 6, 5, 5}));
    EXPECT_EQ(unit, concat({aud, sps, pps, sei, idr, idr2}));

    // A 4-byte start code stays whole in front of its slice.
    const Bytes slice = {0, 0, 0, 1, 0x41, 0x9a};
    auto delta = concat({aud, slice});
    ASSERT_TRUE(skai::insert_before_first_vcl(delta, sei));
    EXPECT_EQ(nal_types(delta), (std::vector<std::uint8_t>{9, 6, 1}));
    EXPECT_EQ(delta, concat({aud, sei, slice}));
}

TEST(DetectionSei, LeavesUnitWithoutSliceUnchanged) {
    const Bytes original = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x68, 0xce};
    auto unit = original;
    const auto sei = skai::encode_user_data_sei_nal(skai::kDetectionSeiUuid, "{}");
    EXPECT_FALSE(skai::insert_before_first_vcl(unit, sei));
    EXPECT_EQ(unit, original);
}

TEST(DetectionSei, NormalizesBoxesToTopLeftSizeWithFourDecimals) {
    const auto box = skai::normalize_sei_box(128, 72, 384, 432, 1280, 720, "person", 0.875f);
    EXPECT_DOUBLE_EQ(box.x, 0.1);
    EXPECT_DOUBLE_EQ(box.y, 0.1);
    EXPECT_DOUBLE_EQ(box.w, 0.2);
    EXPECT_DOUBLE_EQ(box.h, 0.5);
    EXPECT_EQ(box.class_name, "person");
    EXPECT_DOUBLE_EQ(box.score, 0.875);

    const auto third = skai::normalize_sei_box(0, 0, 100, 200, 300, 300, "car", 0.12345f);
    EXPECT_DOUBLE_EQ(third.w, 0.3333);
    EXPECT_DOUBLE_EQ(third.h, 0.6667);
    EXPECT_DOUBLE_EQ(third.score, 0.1235);

    const auto clipped = skai::normalize_sei_box(-10, -5, 1300, 800, 1280, 720, "bus", 1.2f);
    EXPECT_DOUBLE_EQ(clipped.x, 0.0);
    EXPECT_DOUBLE_EQ(clipped.y, 0.0);
    EXPECT_DOUBLE_EQ(clipped.w, 1.0);
    EXPECT_DOUBLE_EQ(clipped.h, 1.0);
    EXPECT_DOUBLE_EQ(clipped.score, 1.0);
}

TEST(DetectionSei, ComputesDtInNinetyKilohertzTicks) {
    EXPECT_EQ(skai::sei_dt_ticks(1'040'000'000, 1'000'000'000), 3600U);
    EXPECT_EQ(skai::sei_dt_ticks(1'000'000'000, 1'000'000'000), 0U);
    EXPECT_EQ(skai::sei_dt_ticks(1'000'005'556, 1'000'000'000), 1U);
    EXPECT_EQ(skai::sei_dt_ticks(1'000'005'555, 1'000'000'000), 0U);
    EXPECT_FALSE(skai::sei_dt_ticks(1'000'000'000, 1'000'000'001));
}

TEST(DetectionSei, SerializesResultsAndEmptyResults) {
    EXPECT_EQ(skai::detection_sei_json({{0, {}}}), R"({"v":1,"r":[{"dt":0,"b":[]}]})");
    std::vector<skai::SeiResult> results{
        {7218, {skai::normalize_sei_box(128, 72, 384, 432, 1280, 720, "person", 0.875f)}},
        {3610, {skai::normalize_sei_box(0, 0, 100, 200, 300, 300, "tv\"x\\", 0.5f),
                skai::normalize_sei_box(0, 0, 1280, 720, 1280, 720, "car", 1.0f)}}};
    EXPECT_EQ(skai::detection_sei_json(results),
              R"({"v":1,"r":[{"dt":7218,"b":[[0.1,0.1,0.2,0.5,"person",0.875]]},)"
              R"({"dt":3610,"b":[[0,0,0.3333,0.6667,"tv\"x\\",0.5],[0,0,1,1,"car",1]]}]})");
}

namespace {

struct EncodedUnit {
    Bytes bytes;
    std::uint64_t pts_ns = 0;
};

std::vector<EncodedUnit> encode_test_stream(int frames) {
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

int decode_frame_count(const std::vector<EncodedUnit>& units) {
    GError* parse_error = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "appsrc name=in is-live=false format=time"
        " caps=video/x-h264,stream-format=byte-stream,alignment=au"
        " ! h264parse ! avdec_h264 ! appsink name=out sync=false",
        &parse_error);
    if (parse_error) {
        ADD_FAILURE() << parse_error->message;
        g_error_free(parse_error);
    }
    if (!pipeline) return -1;
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "in");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    for (const auto& unit : units) {
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, unit.bytes.size(), nullptr);
        gst_buffer_fill(buffer, 0, unit.bytes.data(), unit.bytes.size());
        GST_BUFFER_PTS(buffer) = unit.pts_ns;
        gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
    }
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    int decoded = 0;
    while (GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND)) {
        ++decoded;
        gst_sample_unref(sample);
    }
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* error_message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (error_message) {
        ADD_FAILURE() << "decoder reported an error";
        gst_message_unref(error_message);
        decoded = -1;
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return decoded;
}

} // namespace

TEST(DetectionSei, StreamWithInsertedSeiDecodesAndViewerRecoversSourcePts) {
    constexpr int kFrames = 30;
    auto units = encode_test_stream(kFrames);
    ASSERT_EQ(units.size(), static_cast<std::size_t>(kFrames));
    ASSERT_EQ(nal_types(units[0].bytes).front(), 9) << "test stream should carry an AUD";

    // Each unit carries the two previous frames' results, oldest first.
    std::vector<std::vector<std::uint64_t>> sources(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        std::vector<skai::SeiResult> results;
        for (std::size_t back = std::min<std::size_t>(i, 2); back > 0; --back) {
            const auto source_pts = units[i - back].pts_ns + 1'234;
            sources[i].push_back(source_pts);
            const auto dt = skai::sei_dt_ticks(units[i].pts_ns, source_pts);
            ASSERT_TRUE(dt);
            results.push_back({*dt, {skai::normalize_sei_box(10, 20, 110, 220, 320, 240,
                                                             "person", 0.9f)}});
        }
        if (i % 5 == 4) results.push_back({0, {}});
        if (i % 5 == 4) sources[i].push_back(units[i].pts_ns);
        const auto sei = skai::encode_user_data_sei_nal(
            skai::kDetectionSeiUuid, skai::detection_sei_json(results));
        ASSERT_TRUE(skai::insert_before_first_vcl(units[i].bytes, sei));
    }

    for (std::size_t i = 0; i < units.size(); ++i) {
        std::optional<ParsedSei> found;
        for (const auto& nal : split_annex_b(units[i].bytes)) {
            if (auto sei = parse_user_data_sei(nal)) found = sei;
        }
        ASSERT_TRUE(found) << "unit " << i;
        EXPECT_EQ(found->uuid, skai::kDetectionSeiUuid);
        EXPECT_TRUE(found->trailing_bits_ok);
        const auto dts = dt_values(found->payload);
        ASSERT_EQ(dts.size(), sources[i].size()) << found->payload;
        for (std::size_t r = 0; r < dts.size(); ++r) {
            const auto recovered = static_cast<double>(units[i].pts_ns) - dts[r] * 1e9 / 90000.0;
            EXPECT_LE(std::abs(recovered - static_cast<double>(sources[i][r])), 1e9 / 180000.0)
                << "unit " << i << " result " << r;
        }
    }

    EXPECT_EQ(decode_frame_count(units), kFrames);
}
