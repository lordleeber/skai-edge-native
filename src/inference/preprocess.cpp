#include "skai/inference/preprocess.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skai {
namespace {

bool validate_image(const BgrImageView& image, std::string& error) {
    if (!image.data || image.width <= 0 || image.height <= 0 ||
        image.width > std::numeric_limits<int>::max() / 3) {
        error = "BGR image must have non-null data and positive dimensions";
        return false;
    }
    const auto row_bytes = static_cast<std::size_t>(image.width) * 3;
    if (image.stride < 0 || static_cast<std::size_t>(image.stride) < row_bytes) {
        error = "BGR image stride is shorter than a packed row";
        return false;
    }
    const auto rows_before_last = static_cast<std::size_t>(image.height - 1);
    const auto stride = static_cast<std::size_t>(image.stride);
    if (rows_before_last > (std::numeric_limits<std::size_t>::max() - row_bytes) /
                               stride ||
        image.bytes < rows_before_last * stride + row_bytes) {
        error = "BGR image buffer is shorter than its dimensions and stride";
        return false;
    }
    return true;
}

float pixel(const BgrImageView& image, int x, int y, int channel) {
    return static_cast<float>(image.data[static_cast<std::size_t>(y) * image.stride +
                                         static_cast<std::size_t>(x) * 3 + channel]);
}

float sample_channel(const BgrImageView& image, float x, float y, int channel) {
    const int x_floor = static_cast<int>(std::floor(x));
    const int y_floor = static_cast<int>(std::floor(y));
    const float wx = x - static_cast<float>(x_floor);
    const float wy = y - static_cast<float>(y_floor);
    const int x0 = std::clamp(x_floor, 0, image.width - 1);
    const int x1 = std::clamp(x_floor + 1, 0, image.width - 1);
    const int y0 = std::clamp(y_floor, 0, image.height - 1);
    const int y1 = std::clamp(y_floor + 1, 0, image.height - 1);
    const float upper = pixel(image, x0, y0, channel) * (1.0f - wx) +
                        pixel(image, x1, y0, channel) * wx;
    const float lower = pixel(image, x0, y1, channel) * (1.0f - wx) +
                        pixel(image, x1, y1, channel) * wx;
    return upper * (1.0f - wy) + lower * wy;
}

} // namespace

bool make_preprocess_plan(const BgrImageView& image, int target_width,
                          int target_height, PreprocessPlan& plan,
                          std::string& error) {
    plan = {};
    error.clear();
    if (!validate_image(image, error)) return false;
    if (target_width <= 0 || target_height <= 0) {
        error = "preprocessing target dimensions must be positive";
        return false;
    }
    const double ratio = std::min(static_cast<double>(target_width) / image.width,
                                  static_cast<double>(target_height) / image.height);
    const int resized_width = std::clamp(
        static_cast<int>(std::round(image.width * ratio)), 1, target_width);
    const int resized_height = std::clamp(
        static_cast<int>(std::round(image.height * ratio)), 1, target_height);
    plan.source_width = image.width;
    plan.source_height = image.height;
    plan.width = target_width;
    plan.height = target_height;
    plan.resized_width = resized_width;
    plan.resized_height = resized_height;
    plan.pad_left = (target_width - resized_width) / 2;
    plan.pad_top = (target_height - resized_height) / 2;
    plan.scale = static_cast<float>(ratio);
    return true;
}

bool preprocess_cpu(const BgrImageView& image, const PreprocessPlan& plan,
                    std::vector<float>& output, std::string& error) {
    output.clear();
    error.clear();
    if (!validate_image(image, error)) return false;
    if (image.width != plan.source_width || image.height != plan.source_height ||
        plan.width <= 0 || plan.height <= 0 || plan.scale <= 0.0f ||
        plan.resized_width <= 0 || plan.resized_height <= 0 ||
        plan.pad_left < 0 || plan.pad_top < 0 ||
        plan.pad_left + plan.resized_width > plan.width ||
        plan.pad_top + plan.resized_height > plan.height) {
        error = "preprocessing plan does not match the BGR image";
        return false;
    }
    const auto width = static_cast<std::size_t>(plan.width);
    const auto height = static_cast<std::size_t>(plan.height);
    if (height > std::numeric_limits<std::size_t>::max() / width / 3) {
        error = "preprocessing tensor size overflow";
        return false;
    }
    const auto plane = width * height;
    output.assign(plane * 3, 114.0f / 255.0f);
    for (int y = plan.pad_top; y < plan.pad_top + plan.resized_height; ++y) {
        const float source_y = (static_cast<float>(y - plan.pad_top) + 0.5f) /
                                   plan.scale - 0.5f;
        for (int x = plan.pad_left; x < plan.pad_left + plan.resized_width; ++x) {
            const float source_x = (static_cast<float>(x - plan.pad_left) + 0.5f) /
                                       plan.scale - 0.5f;
            const auto offset = static_cast<std::size_t>(y) * width + x;
            output[offset] = sample_channel(image, source_x, source_y, 2) / 255.0f;
            output[plane + offset] = sample_channel(image, source_x, source_y, 1) / 255.0f;
            output[plane * 2 + offset] = sample_channel(image, source_x, source_y, 0) / 255.0f;
        }
    }
    return true;
}

} // namespace skai
