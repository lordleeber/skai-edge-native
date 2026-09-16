#include "skai/inference/preprocess_cuda.hpp"

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

struct GeneratedImage {
    explicit GeneratedImage(int width, int height, int row_padding)
        : width(width), height(height), stride(width * 3 + row_padding),
          pixels(static_cast<std::size_t>(stride) * height) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto offset = static_cast<std::size_t>(y) * stride + x * 3;
                pixels[offset] = static_cast<std::uint8_t>((x + y) % 256);
                pixels[offset + 1] = static_cast<std::uint8_t>((3 * x + 5 * y) % 256);
                pixels[offset + 2] = static_cast<std::uint8_t>((7 * x + 11 * y) % 256);
            }
        }
    }
    skai::BgrImageView view() const { return {pixels.data(), pixels.size(), width, height, stride}; }
    int width;
    int height;
    int stride;
    std::vector<std::uint8_t> pixels;
};

bool cuda_available() {
    int devices = 0;
    return cudaGetDeviceCount(&devices) == cudaSuccess && devices > 0;
}

} // namespace

TEST(CudaPreprocess, MatchesCpuReferenceForRealYoloEngineAndMeasuresLatency) {
    if (!cuda_available()) GTEST_SKIP() << "CUDA device unavailable";
    ASSERT_TRUE(std::filesystem::exists(SKAI_YOLO_ENGINE));
    ASSERT_TRUE(std::filesystem::exists(SKAI_YOLO_ONNX));
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    std::string error;
    ASSERT_TRUE(bootstrap.initialize_standard_plugins(error)) << error;
    skai::TensorRtEngine engine(logger);
    ASSERT_TRUE(engine.load(SKAI_YOLO_ENGINE, error)) << error;
    ASSERT_EQ(engine.tensors()[0].name, "images");
    EXPECT_EQ(engine.tensors()[0].shape,
              (std::vector<std::int64_t>{1, 3, 640, 640}));
    EXPECT_EQ(engine.tensors()[0].data_type, "FP32");

    skai::CudaPreprocessor cuda;
    for (const auto& shape : {std::pair{1280, 720}, std::pair{641, 359}}) {
        GeneratedImage image(shape.first, shape.second, 7);
        skai::PreprocessPlan plan;
        skai::PreprocessTiming timing;
        ASSERT_TRUE(cuda.run(image.view(), engine, "images", plan, timing, error)) << error;
        EXPECT_GT(timing.gpu_ms, 0.0);
        EXPECT_GT(timing.wall_ms, 0.0);
        EXPECT_TRUE(std::isfinite(timing.gpu_ms));
        std::vector<float> reference;
        ASSERT_TRUE(skai::preprocess_cpu(image.view(), plan, reference, error)) << error;
        std::vector<float> device_result(reference.size());
        ASSERT_EQ(cudaMemcpy(device_result.data(), engine.device_buffer("images"),
                             device_result.size() * sizeof(float),
                             cudaMemcpyDeviceToHost), cudaSuccess);
        float max_difference = 0.0f;
        for (std::size_t index = 0; index < reference.size(); ++index) {
            max_difference = std::max(max_difference,
                                      std::abs(reference[index] - device_result[index]));
        }
        EXPECT_LT(max_difference, 1e-4f) << "source=" << shape.first << "x" << shape.second;
        if (shape.first == 641) {
            ASSERT_EQ(plan.resized_height, 358);
            const auto last_row = static_cast<std::size_t>(plan.pad_top +
                                                           plan.resized_height - 1);
            const auto offset = last_row * plan.width + plan.pad_left;
            // At the last output row, source y is nearly 358; red at (0, 358) is 98.
            EXPECT_NEAR(device_result[offset], 98.0f / 255.0f, 1e-4f);
        }
    }
}

TEST(CudaPreprocess, RejectsInvalidInputAndWorksAfterEngineReload) {
    if (!cuda_available()) GTEST_SKIP() << "CUDA device unavailable";
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    std::string error;
    ASSERT_TRUE(bootstrap.initialize_standard_plugins(error)) << error;
    skai::TensorRtEngine engine(logger);
    ASSERT_TRUE(engine.load(SKAI_YOLO_ENGINE, error)) << error;
    GeneratedImage image(16, 9, 3);
    skai::CudaPreprocessor cuda;
    skai::PreprocessPlan plan;
    skai::PreprocessTiming timing;
    EXPECT_FALSE(cuda.run(image.view(), engine, "missing", plan, timing, error));
    EXPECT_NE(error.find("input"), std::string::npos);
    auto truncated = image.view();
    truncated.bytes = 1;
    EXPECT_FALSE(cuda.run(truncated, engine, "images", plan, timing, error));
    EXPECT_NE(error.find("buffer"), std::string::npos);
    ASSERT_TRUE(cuda.run(image.view(), engine, "images", plan, timing, error)) << error;
    engine.unload();
    ASSERT_TRUE(engine.load(SKAI_YOLO_ENGINE, error)) << error;
    ASSERT_TRUE(cuda.run(image.view(), engine, "images", plan, timing, error)) << error;
}
