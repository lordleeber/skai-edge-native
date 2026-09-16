#include "skai/video/annotator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

skai::Frame make_frame(int width = 160, int height = 100, int padding = 7) {
    skai::Frame frame;
    frame.sequence = 7;
    frame.timestamp = std::chrono::steady_clock::now();
    frame.pts_ns = 123456;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 3 + padding;
    frame.bgr.resize(static_cast<std::size_t>(frame.stride) * height, 0);
    for (int y = 0; y < height; ++y) {
        std::fill(frame.bgr.begin() + static_cast<std::size_t>(y) * frame.stride +
                      width * 3,
                  frame.bgr.begin() + static_cast<std::size_t>(y + 1) * frame.stride,
                  0xAA);
    }
    return frame;
}

skai::DetectionResult make_detection() {
    skai::DetectionResult result;
    result.frame_sequence = 7;
    result.detections.push_back({0, 0.9f, 20, 30, 100, 80});
    return result;
}

std::uint8_t pixel(const skai::Frame& frame, int x, int y, int channel) {
    return frame.bgr[static_cast<std::size_t>(y) * frame.stride + x * 3 + channel];
}

} // namespace

TEST(Annotator, DrawsBoxAndLabelOnCopyWithoutChangingInputOrDetections) {
    const auto source = make_frame();
    const auto original_pixels = source.bgr;
    const auto detections = make_detection();
    const auto original_detection = detections.detections[0];
    skai::AnnotationOptions options;
    options.box_thickness = 1;
    skai::Frame output;
    std::string error;
    ASSERT_TRUE(skai::annotate_frame(source, detections, {"person"}, options,
                                      output, error)) << error;
    EXPECT_EQ(output.sequence, source.sequence);
    EXPECT_EQ(output.timestamp, source.timestamp);
    EXPECT_EQ(output.pts_ns, source.pts_ns);
    EXPECT_EQ(output.width, source.width);
    EXPECT_EQ(output.height, source.height);
    EXPECT_EQ(output.stride, source.stride);
    EXPECT_EQ(source.bgr, original_pixels);
    EXPECT_FLOAT_EQ(detections.detections[0].confidence,
                    original_detection.confidence);
    EXPECT_FLOAT_EQ(detections.detections[0].x1, original_detection.x1);
    EXPECT_EQ(pixel(output, 20, 30, 1), 255);
    EXPECT_EQ(pixel(output, 60, 50, 1), 0);
    EXPECT_EQ(output.bgr[160 * 3], 0xAA);
    bool label_changed = false;
    for (int y = 0; y < 30; ++y) {
        for (int x = 20; x < 100; ++x) {
            label_changed |= pixel(output, x, y, 1) != 0;
        }
    }
    EXPECT_TRUE(label_changed);
}

TEST(Annotator, FormatsClassNameAndConfidence) {
    EXPECT_EQ(skai::detection_label({0, 0.9f, 0, 0, 1, 1}, {"person"}),
              "person 0.90");
    EXPECT_EQ(skai::detection_label({3, 0.125f, 0, 0, 1, 1}, {}),
              "class_3 0.12");
}

TEST(Annotator, CanBeDisabledWithoutChangingPixelsOrMetadata) {
    const auto source = make_frame();
    skai::DetectionResult detections;
    detections.frame_sequence = source.sequence + 1;
    skai::AnnotationOptions options;
    options.enabled = false;
    skai::Frame output;
    std::string error;
    ASSERT_TRUE(skai::annotate_frame(source, detections, {"person"}, options,
                                      output, error)) << error;
    EXPECT_EQ(output.bgr, source.bgr);
    EXPECT_EQ(output.sequence, source.sequence);
    EXPECT_EQ(output.timestamp, source.timestamp);
    EXPECT_EQ(output.pts_ns, source.pts_ns);
}

TEST(Annotator, ReportsOpenCvDrawingErrorsWithoutThrowing) {
    const auto source = make_frame();
    const auto detections = make_detection();
    skai::AnnotationOptions options;
    options.box_thickness = 32768;
    skai::Frame output;
    std::string error;
    EXPECT_NO_THROW(EXPECT_FALSE(skai::annotate_frame(
        source, detections, {"person"}, options, output, error)));
    EXPECT_FALSE(error.empty());

    options.box_thickness = 1;
    options.font_scale = std::numeric_limits<double>::max();
    EXPECT_NO_THROW(EXPECT_FALSE(skai::annotate_frame(
        source, detections, {"person"}, options, output, error)));
    EXPECT_FALSE(error.empty());
}

TEST(Annotator, DrawsOptionalFpsAndInferenceTime) {
    const auto source = make_frame();
    skai::DetectionResult detections;
    detections.frame_sequence = source.sequence;
    skai::AnnotationOptions options;
    options.show_metrics = true;
    options.fps = 30.0;
    options.inference_ms = 5.2;
    skai::Frame output;
    std::string error;
    ASSERT_TRUE(skai::annotate_frame(source, detections, {}, options, output,
                                      error)) << error;
    EXPECT_NE(output.bgr, source.bgr);
    EXPECT_EQ(source.bgr, make_frame().bgr);
    options.enabled = false;
    ASSERT_TRUE(skai::annotate_frame(source, detections, {}, options, output,
                                      error)) << error;
    EXPECT_EQ(output.bgr, source.bgr);
}

TEST(Annotator, RejectsInvalidFrameMismatchedSequenceAndAliasedOutput) {
    auto source = make_frame();
    auto detections = make_detection();
    skai::Frame output;
    std::string error;
    source.bgr.resize(1);
    EXPECT_FALSE(skai::annotate_frame(source, detections, {}, {}, output, error));
    EXPECT_NE(error.find("buffer"), std::string::npos);
    source = make_frame();
    detections.frame_sequence = 8;
    EXPECT_FALSE(skai::annotate_frame(source, detections, {}, {}, output, error));
    EXPECT_NE(error.find("sequence"), std::string::npos);
    detections.frame_sequence = 7;
    EXPECT_FALSE(skai::annotate_frame(source, detections, {}, {}, source, error));
    EXPECT_NE(error.find("distinct"), std::string::npos);
}
