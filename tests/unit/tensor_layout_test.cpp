#include "skai/inference/tensor_layout.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

namespace {

skai::TensorDescription input(std::vector<std::int64_t> shape = {1, 3, 640, 640}) {
    return {"images", true, std::move(shape), "FP32", 4, true, true};
}

skai::TensorDescription output() {
    return {"detections", false, {1, 84, 8400}, "FP16", 2, true, true};
}

} // namespace

TEST(TensorLayout, CalculatesBindingBytesAndReportsMetadata) {
    std::vector<skai::TensorInfo> result;
    std::string error;
    ASSERT_TRUE(skai::validate_tensor_layout({input(), output()}, result, error)) << error;
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].bytes, 1U * 3 * 640 * 640 * 4);
    EXPECT_EQ(result[1].bytes, 1U * 84 * 8400 * 2);
    EXPECT_TRUE(result[0].input);
    EXPECT_FALSE(result[1].input);
}

TEST(TensorLayout, RejectsMissingInputOrOutput) {
    std::vector<skai::TensorInfo> result;
    std::string error;
    EXPECT_FALSE(skai::validate_tensor_layout({input()}, result, error));
    EXPECT_NE(error.find("output"), std::string::npos);
    EXPECT_FALSE(skai::validate_tensor_layout({output()}, result, error));
    EXPECT_NE(error.find("input"), std::string::npos);
}

TEST(TensorLayout, RejectsDynamicInvalidAndOverflowingBindings) {
    std::vector<skai::TensorInfo> result;
    std::string error;
    EXPECT_FALSE(skai::validate_tensor_layout({input({1, 3, -1, 640}), output()}, result, error));
    EXPECT_NE(error.find("images"), std::string::npos);
    EXPECT_NE(error.find("dynamic"), std::string::npos);
    EXPECT_FALSE(skai::validate_tensor_layout({input({1, 3, 0, 640}), output()}, result, error));
    EXPECT_NE(error.find("images"), std::string::npos);
    EXPECT_FALSE(skai::validate_tensor_layout(
        {input({std::numeric_limits<std::int64_t>::max(), 2}), output()}, result, error));
    EXPECT_NE(error.find("overflow"), std::string::npos);
}

TEST(TensorLayout, RejectsDuplicateNonlinearAndHostBindings) {
    std::vector<skai::TensorInfo> result;
    std::string error;
    auto other = output();
    other.name = "images";
    EXPECT_FALSE(skai::validate_tensor_layout({input(), other}, result, error));
    EXPECT_NE(error.find("duplicate"), std::string::npos);
    other = output();
    other.linear = false;
    EXPECT_FALSE(skai::validate_tensor_layout({input(), other}, result, error));
    EXPECT_NE(error.find("linear"), std::string::npos);
    other = output();
    other.device = false;
    EXPECT_FALSE(skai::validate_tensor_layout({input(), other}, result, error));
    EXPECT_NE(error.find("device"), std::string::npos);
}
