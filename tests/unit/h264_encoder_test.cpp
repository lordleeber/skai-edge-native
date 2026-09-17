#include "skai/core/bounded_queue.hpp"
#include "skai/logging.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/h264_encoder.hpp"
#include "skai/video/h264_encoder_module.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

namespace {

skai::Frame frame(std::uint64_t sequence, std::uint64_t pts_ns = 0,
                  int width = 32, int height = 24) {
    skai::Frame result;
    result.sequence = sequence;
    result.pts_ns = pts_ns;
    result.width = width;
    result.height = height;
    result.stride = result.width * 3;
    result.bgr.resize(static_cast<std::size_t>(result.stride) * result.height, 0);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            const auto offset = static_cast<std::size_t>(y * result.stride + x * 3);
            result.bgr[offset] = static_cast<std::uint8_t>(x * 7);
            result.bgr[offset + 1] = static_cast<std::uint8_t>(y * 9);
            result.bgr[offset + 2] = static_cast<std::uint8_t>((x + y) * 5);
        }
    }
    return result;
}

bool starts_with_annex_b_start_code(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 &&
           (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1));
}

} // namespace

TEST(H264Encoder, RejectsInvalidConfigurationBeforeStartingWorker) {
    skai::BoundedQueue<skai::Frame> input(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> output(2);
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::H264Encoder encoder(input, output, logger);
    std::string error;

    EXPECT_FALSE(encoder.start({0, 30, 30, 1}, error));
    EXPECT_NE(error.find("positive"), std::string::npos);
    EXPECT_FALSE(output.is_shutdown());
}

TEST(H264Encoder, EncodesBoundedAnnexBAccessUnitsAndReportsMetrics) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::BoundedQueue<skai::Frame> input(4);
    skai::BoundedQueue<skai::EncodedAccessUnit> output(1);
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto status = std::make_shared<skai::RuntimeStatus>();
    skai::H264Encoder encoder(input, output, logger, status);
    ASSERT_TRUE(encoder.start({}, error)) << error;

    ASSERT_TRUE(input.push(frame(41, 33'000'000)));
    auto unit = output.pop_for(std::chrono::seconds(3));
    ASSERT_TRUE(unit.has_value()) << logs.str() << encoder.metrics().last_error;
    EXPECT_NE(unit->pts_ns, 0U);
    EXPECT_TRUE(unit->keyframe);
    EXPECT_TRUE(starts_with_annex_b_start_code(unit->bytes));
    ASSERT_TRUE(status->snapshot().encoder.has_value());
    EXPECT_GE(status->snapshot().encoder->access_units_encoded, 1U);

    ASSERT_TRUE(input.push(frame(42, 66'000'000)));
    ASSERT_TRUE(input.push(frame(43, 99'000'000)));
    ASSERT_TRUE(input.push(frame(44, 132'000'000)));
    ASSERT_TRUE(input.push(frame(45, 165'000'000)));
    for (int attempt = 0; attempt < 300 &&
                          encoder.metrics().access_units_encoded < 4; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    encoder.stop();

    const auto metrics = encoder.metrics();
    EXPECT_EQ(metrics.frames_submitted + metrics.appsrc_pressure_dropped, 5U);
    EXPECT_GE(metrics.access_units_encoded, 4U);
    EXPECT_GT(metrics.bytes_encoded, 0U);
    EXPECT_GE(metrics.access_units_dropped, 1U);
    EXPECT_LT(metrics.last_access_unit_age_ms, 5'000);
    EXPECT_TRUE(metrics.last_error.empty());
    EXPECT_NE(skai::serialize_h264_encoder_metrics(metrics).find("access_units_encoded"),
              std::string::npos);
}

TEST(H264Encoder, RebuildsForDimensionChangesAndEmitsNewKeyframe) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::BoundedQueue<skai::Frame> input(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> output(4);
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::H264Encoder encoder(input, output, logger);
    ASSERT_TRUE(encoder.start({}, error)) << error;
    ASSERT_TRUE(input.push(frame(1)));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(3)).has_value());
    ASSERT_TRUE(input.push(frame(2, 0, 16, 24)));
    auto rebuilt = output.pop_for(std::chrono::seconds(3));
    ASSERT_TRUE(rebuilt.has_value()) << logs.str() << encoder.metrics().last_error;
    encoder.stop();
    EXPECT_TRUE(rebuilt->keyframe);
    EXPECT_EQ(encoder.metrics().frames_rejected, 0U);
    EXPECT_EQ(encoder.metrics().pipeline_rebuilds, 1U);
    EXPECT_TRUE(encoder.metrics().last_error.empty());
}

TEST(H264Encoder, RebaseForeignPtsOntoMonotonicEncoderTimeline) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::BoundedQueue<skai::Frame> input(3);
    skai::BoundedQueue<skai::EncodedAccessUnit> output(3);
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::H264Encoder encoder(input, output, logger);
    ASSERT_TRUE(encoder.start({}, error)) << error;
    auto first = frame(1, 9'000'000'000ULL);
    first.timestamp = std::chrono::steady_clock::now();
    auto second = frame(2, 1);
    second.timestamp = first.timestamp + std::chrono::milliseconds(33);
    ASSERT_TRUE(input.push(std::move(first)));
    ASSERT_TRUE(input.push(std::move(second)));
    auto first_unit = output.pop_for(std::chrono::seconds(3));
    auto second_unit = output.pop_for(std::chrono::seconds(3));
    ASSERT_TRUE(first_unit.has_value()) << logs.str() << encoder.metrics().last_error;
    ASSERT_TRUE(second_unit.has_value()) << logs.str() << encoder.metrics().last_error;
    encoder.stop();
    EXPECT_LT(first_unit->pts_ns, second_unit->pts_ns);
}

TEST(H264EncoderModule, ConnectsAnnotatedFramesToProductionAccessUnitQueue) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::BoundedQueue<skai::Frame> annotated_frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> encoded_access_units(2);
    std::ostringstream logs;
    skai::Logger logger(logs);
    auto status = std::make_shared<skai::RuntimeStatus>();
    skai::H264EncoderModule module(annotated_frames, encoded_access_units, logger, status);

    ASSERT_TRUE(module.initialize(skai::Config{}));
    ASSERT_TRUE(module.start()) << module.last_error();
    ASSERT_TRUE(annotated_frames.push(frame(17)));
    ASSERT_TRUE(encoded_access_units.pop_for(std::chrono::seconds(3)).has_value());
    ASSERT_TRUE(status->snapshot().encoder.has_value());
    module.stop();
    module.wait();
    EXPECT_FALSE(status->snapshot().encoder.has_value());
}
