#include "skai/inference/yolo_detector.hpp"

#include <gtest/gtest.h>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> reference_image() {
    std::vector<std::uint8_t> bytes(640 * 640 * 3);
    for (int y = 0; y < 640; ++y) {
        for (int x = 0; x < 640; ++x) {
            const auto offset = static_cast<std::size_t>(y * 640 + x) * 3;
            bytes[offset] = static_cast<std::uint8_t>((x + y) % 256);
            bytes[offset + 1] = static_cast<std::uint8_t>((2 * x + y) % 256);
            bytes[offset + 2] = static_cast<std::uint8_t>((x + 2 * y) % 256);
        }
    }
    return bytes;
}

} // namespace

TEST(YoloDetector, RunsRealEngineAndReturnsDeterministicDetectionsAndTiming) {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        GTEST_SKIP() << "CUDA device unavailable";
    }
    ASSERT_TRUE(std::filesystem::exists(SKAI_YOLO_ENGINE));
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    skai::YoloPostprocessConfig config;
    config.confidence_threshold = 0.1f;
    skai::YoloDetector detector(logger, bootstrap, config);
    std::string error;
    ASSERT_TRUE(detector.load(SKAI_YOLO_ENGINE, error)) << error;
    ASSERT_TRUE(detector.loaded());
    const auto bytes = reference_image();
    const skai::BgrImageView image{bytes.data(), bytes.size(), 640, 640, 640 * 3};
    skai::DetectionResult first;
    skai::InferenceTiming first_timing;
    ASSERT_TRUE(detector.run(image, 17, first, first_timing, error)) << error;
    EXPECT_EQ(first.frame_sequence, 17U);
    ASSERT_FALSE(first.detections.empty());
    // ONNX Runtime's highest-scoring prediction for this image is class 25,
    // confidence 0.227, and a box spanning roughly (1.5, 2.2) to (637.5, 637.4).
    EXPECT_EQ(first.detections[0].class_id, 25);
    EXPECT_NEAR(first.detections[0].confidence, 0.227177f, 0.03f);
    EXPECT_NEAR(first.detections[0].x1, 1.518f, 3.0f);
    EXPECT_NEAR(first.detections[0].y1, 2.189f, 3.0f);
    EXPECT_NEAR(first.detections[0].x2, 637.463f, 3.0f);
    EXPECT_NEAR(first.detections[0].y2, 637.410f, 3.0f);
    EXPECT_GT(first_timing.preprocess.gpu_ms, 0.0);
    EXPECT_GT(first_timing.inference_gpu_ms, 0.0);
    EXPECT_GT(first_timing.inference_wall_ms, 0.0);
    EXPECT_TRUE(std::isfinite(first_timing.postprocess_wall_ms));

    // Reference values from yolo11s.onnx on the generated image with RGB / 255
    // input, evaluated independently with ONNX Runtime's CPU provider.
    ASSERT_EQ(detector.engine().tensors()[1].shape,
              (std::vector<std::int64_t>{1, 84, 8400}));
    std::vector<float> raw(84 * 8400);
    ASSERT_EQ(cudaMemcpy(raw.data(), detector.engine().device_buffer("output0"),
                         raw.size() * sizeof(float), cudaMemcpyDeviceToHost),
              cudaSuccess);
    EXPECT_NEAR(raw[0], 5.74137f, 1.0f);
    EXPECT_NEAR(raw[8400], 6.19486f, 1.0f);
    EXPECT_NEAR(raw[16800], 11.35339f, 1.0f);
    EXPECT_NEAR(raw[25200], 12.12368f, 1.0f);
    EXPECT_NEAR(raw[29 * 8400 + 8191], 0.227177f, 0.03f);

    skai::DetectionResult second;
    skai::InferenceTiming second_timing;
    ASSERT_TRUE(detector.run(image, 18, second, second_timing, error)) << error;
    EXPECT_EQ(second.frame_sequence, 18U);
    ASSERT_EQ(second.detections.size(), first.detections.size());
    for (std::size_t index = 0; index < first.detections.size(); ++index) {
        const auto& a = first.detections[index];
        const auto& b = second.detections[index];
        EXPECT_EQ(a.class_id, b.class_id);
        EXPECT_NEAR(a.confidence, b.confidence, 1e-6f);
        EXPECT_NEAR(a.x1, b.x1, 1e-4f);
        EXPECT_NEAR(a.y1, b.y1, 1e-4f);
        EXPECT_NEAR(a.x2, b.x2, 1e-4f);
        EXPECT_NEAR(a.y2, b.y2, 1e-4f);
    }
}

TEST(YoloDetector, RejectsMissingEngineAndInvalidFrame) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    skai::YoloDetector detector(logger, bootstrap);
    std::string error;
    EXPECT_FALSE(detector.load("/tmp/skai-step10-missing.engine", error));
    EXPECT_FALSE(detector.loaded());
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        GTEST_SKIP() << "CUDA device unavailable";
    }
    ASSERT_TRUE(detector.load(SKAI_YOLO_ENGINE, error)) << error;
    const auto bytes = reference_image();
    const skai::BgrImageView invalid{bytes.data(), 1, 640, 640, 640 * 3};
    skai::DetectionResult result;
    skai::InferenceTiming timing;
    EXPECT_FALSE(detector.run(invalid, 1, result, timing, error));
    EXPECT_NE(error.find("buffer"), std::string::npos);
}

TEST(YoloDetector, MatchesBlankOnnxReferenceWithoutFalseDetections) {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        GTEST_SKIP() << "CUDA device unavailable";
    }
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    skai::YoloDetector detector(logger, bootstrap);
    std::string error;
    ASSERT_TRUE(detector.load(SKAI_YOLO_ENGINE, error)) << error;
    const std::vector<std::uint8_t> black(640 * 640 * 3, 0);
    const skai::BgrImageView image{black.data(), black.size(), 640, 640, 640 * 3};
    skai::DetectionResult result;
    skai::InferenceTiming timing;
    ASSERT_TRUE(detector.run(image, 19, result, timing, error)) << error;
    EXPECT_EQ(result.frame_sequence, 19U);
    EXPECT_TRUE(result.detections.empty());

    // ONNX Runtime CPU reference on all-zero RGB input: first xywh is
    // (7.790, 7.461, 16.301, 17.837), and the largest score is 0.000147.
    std::vector<float> raw(84 * 8400);
    ASSERT_EQ(cudaMemcpy(raw.data(), detector.engine().device_buffer("output0"),
                         raw.size() * sizeof(float), cudaMemcpyDeviceToHost),
              cudaSuccess);
    EXPECT_NEAR(raw[0], 7.790f, 1.0f);
    EXPECT_NEAR(raw[8400], 7.461f, 1.0f);
    EXPECT_NEAR(raw[16800], 16.301f, 1.0f);
    EXPECT_NEAR(raw[25200], 17.837f, 1.0f);
    const auto largest_score = *std::max_element(raw.begin() + 4 * 8400,
                                                  raw.end());
    EXPECT_LT(largest_score, 0.001f);
}
