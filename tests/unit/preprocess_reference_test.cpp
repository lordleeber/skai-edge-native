#include "skai/inference/preprocess.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

skai::BgrImageView view(const std::vector<std::uint8_t>& bytes,
                        int width, int height, int stride) {
    return {bytes.data(), bytes.size(), width, height, stride};
}

} // namespace

TEST(PreprocessReference, PlansCenteredLetterboxForWideFrame) {
    std::vector<std::uint8_t> bytes(1920 * 1080 * 3);
    skai::PreprocessPlan plan;
    std::string error;
    ASSERT_TRUE(skai::make_preprocess_plan(view(bytes, 1920, 1080, 1920 * 3),
                                            640, 640, plan, error)) << error;
    EXPECT_EQ(plan.width, 640);
    EXPECT_EQ(plan.height, 640);
    EXPECT_EQ(plan.resized_width, 640);
    EXPECT_EQ(plan.resized_height, 360);
    EXPECT_EQ(plan.pad_left, 0);
    EXPECT_EQ(plan.pad_top, 140);
    EXPECT_NEAR(plan.scale, 1.0 / 3.0, 1e-6);
}

TEST(PreprocessReference, ConvertsBgrToNormalizedRgbNchwWithBilinearResize) {
    const std::vector<std::uint8_t> bytes = {10, 20, 30, 110, 120, 130};
    const auto image = view(bytes, 2, 1, 6);
    skai::PreprocessPlan plan;
    std::string error;
    ASSERT_TRUE(skai::make_preprocess_plan(image, 4, 4, plan, error)) << error;
    EXPECT_EQ(plan.pad_top, 1);
    std::vector<float> output;
    ASSERT_TRUE(skai::preprocess_cpu(image, plan, output, error)) << error;
    ASSERT_EQ(output.size(), 3U * 4 * 4);
    const std::array<int, 4> red = {30, 55, 105, 130};
    const std::array<int, 4> green = {20, 45, 95, 120};
    const std::array<int, 4> blue = {10, 35, 85, 110};
    for (int x = 0; x < 4; ++x) {
        EXPECT_NEAR(output[4 + x], red[x] / 255.0f, 1e-6);
        EXPECT_NEAR(output[16 + 4 + x], green[x] / 255.0f, 1e-6);
        EXPECT_NEAR(output[32 + 4 + x], blue[x] / 255.0f, 1e-6);
        EXPECT_NEAR(output[x], 114.0f / 255.0f, 1e-6);
    }
}

TEST(PreprocessReference, HonorsRowStrideAndRejectsTruncatedInput) {
    const std::vector<std::uint8_t> bytes = {
        1, 2, 3, 4, 5, 6, 250, 251,
        7, 8, 9, 10, 11, 12,
    };
    const auto image = view(bytes, 2, 2, 8);
    skai::PreprocessPlan plan;
    std::string error;
    ASSERT_TRUE(skai::make_preprocess_plan(image, 2, 2, plan, error)) << error;
    std::vector<float> output;
    ASSERT_TRUE(skai::preprocess_cpu(image, plan, output, error)) << error;
    EXPECT_NEAR(output[2], 9.0f / 255.0f, 1e-6);
    EXPECT_NEAR(output[3], 12.0f / 255.0f, 1e-6);
    const auto truncated = view(bytes, 2, 2, 9);
    EXPECT_FALSE(skai::make_preprocess_plan(truncated, 2, 2, plan, error));
    EXPECT_NE(error.find("buffer"), std::string::npos);
}

TEST(PreprocessReference, RejectsInvalidGeometryAndMismatchedPlan) {
    const std::vector<std::uint8_t> bytes(12);
    skai::PreprocessPlan plan;
    std::string error;
    EXPECT_FALSE(skai::make_preprocess_plan(view(bytes, 0, 2, 6), 2, 2, plan, error));
    EXPECT_FALSE(skai::make_preprocess_plan(view(bytes, 2, 2, 5), 2, 2, plan, error));
    EXPECT_FALSE(skai::make_preprocess_plan(view(bytes, 2, 2, 6), 0, 2, plan, error));
    const auto image = view(bytes, 2, 2, 6);
    ASSERT_TRUE(skai::make_preprocess_plan(image, 2, 2, plan, error));
    std::vector<float> output;
    EXPECT_FALSE(skai::preprocess_cpu(view(bytes, 1, 2, 6), plan, output, error));
    EXPECT_NE(error.find("plan"), std::string::npos);
}
