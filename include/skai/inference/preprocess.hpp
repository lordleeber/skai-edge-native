#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skai {

// A borrowed packed BGR frame; stride may include padding between rows.
struct BgrImageView {
    const std::uint8_t* data = nullptr;
    std::size_t bytes = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
};

// The inverse transform is also needed to restore box coordinates in Step 10.
struct PreprocessPlan {
    int source_width = 0;
    int source_height = 0;
    int width = 0;
    int height = 0;
    int resized_width = 0;
    int resized_height = 0;
    int pad_left = 0;
    int pad_top = 0;
    float scale = 0.0f;
};

bool make_preprocess_plan(const BgrImageView& image, int target_width,
                          int target_height, PreprocessPlan& plan,
                          std::string& error);

// Bilinear letterbox (114), BGR -> RGB, x/255, packed as FP32 NCHW.
bool preprocess_cpu(const BgrImageView& image, const PreprocessPlan& plan,
                    std::vector<float>& output, std::string& error);

} // namespace skai
