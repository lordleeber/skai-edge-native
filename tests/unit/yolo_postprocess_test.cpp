#include "skai/inference/yolo_postprocess.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

skai::PreprocessPlan make_plan(int source_width, int source_height) {
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(source_width) * source_height * 3);
    const skai::BgrImageView image{bytes.data(), bytes.size(), source_width,
                                   source_height, source_width * 3};
    skai::PreprocessPlan plan;
    std::string error;
    EXPECT_TRUE(skai::make_preprocess_plan(image, 640, 640, plan, error)) << error;
    return plan;
}

void set_candidate(std::vector<float>& output, int count, int index,
                   float cx, float cy, float width, float height,
                   float class_zero, float class_one) {
    output[index] = cx;
    output[count + index] = cy;
    output[2 * count + index] = width;
    output[3 * count + index] = height;
    output[4 * count + index] = class_zero;
    output[5 * count + index] = class_one;
}

} // namespace

TEST(YoloPostprocess, FiltersScoresSuppressesSameClassAndRestoresCoordinates) {
    const auto plan = make_plan(1280, 720);
    ASSERT_EQ(plan.pad_top, 140);
    std::vector<float> output(6 * 5);
    set_candidate(output, 5, 0, 100, 200, 40, 20, 0.9f, 0.1f);
    set_candidate(output, 5, 1, 102, 201, 40, 20, 0.8f, 0.1f);
    set_candidate(output, 5, 2, 100, 200, 40, 20, 0.1f, 0.85f);
    set_candidate(output, 5, 3, 600, 350, 50, 50, 0.7f, 0.1f);
    set_candidate(output, 5, 4, 300, 200, 40, 20, 0.1f, 0.2f);

    skai::DetectionResult result;
    std::string error;
    ASSERT_TRUE(skai::postprocess_yolo({output.data(), output.size(), 6, 5},
                                        plan, {}, 42, result, error)) << error;
    EXPECT_EQ(result.frame_sequence, 42U);
    ASSERT_EQ(result.detections.size(), 3U);
    EXPECT_EQ(result.detections[0].class_id, 0);
    EXPECT_NEAR(result.detections[0].confidence, 0.9f, 1e-6f);
    EXPECT_NEAR(result.detections[0].x1, 160.0f, 1e-5f);
    EXPECT_NEAR(result.detections[0].y1, 100.0f, 1e-5f);
    EXPECT_NEAR(result.detections[0].x2, 240.0f, 1e-5f);
    EXPECT_NEAR(result.detections[0].y2, 140.0f, 1e-5f);
    EXPECT_EQ(result.detections[1].class_id, 1);
    EXPECT_EQ(result.detections[2].class_id, 0);

    skai::YoloPostprocessConfig tuned;
    tuned.confidence_threshold = 0.75f;
    tuned.nms_iou_threshold = 0.95f;
    ASSERT_TRUE(skai::postprocess_yolo({output.data(), output.size(), 6, 5},
                                        plan, tuned, 43, result, error)) << error;
    ASSERT_EQ(result.detections.size(), 3U);
    EXPECT_EQ(result.frame_sequence, 43U);
    EXPECT_NEAR(result.detections[2].confidence, 0.8f, 1e-6f);
}

TEST(YoloPostprocess, UsesRoundedResizeDimensionsForBoxRestoration) {
    const auto plan = make_plan(641, 359);
    ASSERT_EQ(plan.resized_width, 640);
    ASSERT_EQ(plan.resized_height, 358);
    std::vector<float> output(5);
    output[0] = 320.0f;
    output[1] = static_cast<float>(plan.pad_top) + 179.0f;
    output[2] = 640.0f;
    output[3] = 358.0f;
    output[4] = 0.9f;

    skai::DetectionResult result;
    std::string error;
    ASSERT_TRUE(skai::postprocess_yolo({output.data(), output.size(), 5, 1},
                                        plan, {}, 9, result, error)) << error;
    ASSERT_EQ(result.detections.size(), 1U);
    EXPECT_NEAR(result.detections[0].x1, 0.0f, 1e-5f);
    EXPECT_NEAR(result.detections[0].y1, 0.0f, 1e-5f);
    EXPECT_NEAR(result.detections[0].x2, 641.0f, 1e-5f);
    EXPECT_NEAR(result.detections[0].y2, 359.0f, 1e-5f);
}

TEST(YoloPostprocess, RejectsInvalidInputsAndKeepsResultsDeterministic) {
    const auto plan = make_plan(640, 640);
    std::vector<float> output(5 * 2);
    output[0] = output[2] = 100.0f;
    output[1] = output[3] = 300.0f;
    output[4] = output[5] = 20.0f;
    output[6] = output[7] = 20.0f;
    output[8] = output[9] = 0.8f;
    skai::DetectionResult result;
    std::string error;
    skai::YoloPostprocessConfig config;
    config.max_detections = 1;
    ASSERT_TRUE(skai::postprocess_yolo({output.data(), output.size(), 5, 2},
                                        plan, config, 3, result, error)) << error;
    ASSERT_EQ(result.detections.size(), 1U);
    EXPECT_EQ(result.detections[0].class_id, 0);

    config.nms_iou_threshold = 1.1f;
    EXPECT_FALSE(skai::postprocess_yolo({output.data(), output.size(), 5, 2},
                                         plan, config, 3, result, error));
    EXPECT_NE(error.find("threshold"), std::string::npos);
    config.nms_iou_threshold = 0.45f;
    EXPECT_FALSE(skai::postprocess_yolo({output.data(), output.size() - 1, 5, 2},
                                         plan, config, 3, result, error));
    EXPECT_NE(error.find("shape"), std::string::npos);
    output[8] = std::numeric_limits<float>::quiet_NaN();
    ASSERT_TRUE(skai::postprocess_yolo({output.data(), output.size(), 5, 2},
                                        plan, config, 3, result, error)) << error;
    ASSERT_EQ(result.detections.size(), 1U);
    EXPECT_TRUE(std::isfinite(result.detections[0].confidence));
}
