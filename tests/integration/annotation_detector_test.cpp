#include "skai/inference/yolo_detector.hpp"
#include "skai/video/annotator.hpp"

#include <gtest/gtest.h>

#include <cuda_runtime_api.h>

#include <cstdint>
#include <sstream>
#include <string>

namespace {

skai::Frame reference_frame() {
    skai::Frame frame;
    frame.sequence = 33;
    frame.pts_ns = 123;
    frame.width = 640;
    frame.height = 640;
    frame.stride = 640 * 3;
    frame.bgr.resize(640 * 640 * 3);
    for (int y = 0; y < 640; ++y) {
        for (int x = 0; x < 640; ++x) {
            const auto offset = static_cast<std::size_t>(y * 640 + x) * 3;
            frame.bgr[offset] = static_cast<std::uint8_t>((x + y) % 256);
            frame.bgr[offset + 1] = static_cast<std::uint8_t>((2 * x + y) % 256);
            frame.bgr[offset + 2] = static_cast<std::uint8_t>((x + 2 * y) % 256);
        }
    }
    return frame;
}

} // namespace

TEST(AnnotationDetector, ProducesAnnotatedFrameWithoutChangingInferenceResult) {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        GTEST_SKIP() << "CUDA device unavailable";
    }
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    skai::YoloPostprocessConfig postprocess;
    postprocess.confidence_threshold = 0.1f;
    skai::YoloDetector detector(logger, bootstrap, postprocess);
    std::string error;
    ASSERT_TRUE(detector.load(SKAI_YOLO_ENGINE, error)) << error;

    const auto frame = reference_frame();
    const auto original_pixels = frame.bgr;
    const skai::BgrImageView image{frame.bgr.data(), frame.bgr.size(),
                                   frame.width, frame.height, frame.stride};
    skai::DetectionResult result;
    skai::InferenceTiming timing;
    ASSERT_TRUE(detector.run(image, frame.sequence, result, timing, error)) << error;
    ASSERT_FALSE(result.detections.empty());
    const auto original_result = result;

    skai::Frame annotated;
    ASSERT_TRUE(skai::annotate_frame(frame, result, {}, {}, annotated, error))
        << error;
    EXPECT_EQ(annotated.sequence, frame.sequence);
    EXPECT_EQ(annotated.pts_ns, frame.pts_ns);
    EXPECT_NE(annotated.bgr, original_pixels);
    EXPECT_EQ(frame.bgr, original_pixels);
    ASSERT_EQ(result.detections.size(), original_result.detections.size());
    for (std::size_t index = 0; index < result.detections.size(); ++index) {
        const auto& before = original_result.detections[index];
        const auto& after = result.detections[index];
        EXPECT_EQ(after.class_id, before.class_id);
        EXPECT_FLOAT_EQ(after.confidence, before.confidence);
        EXPECT_FLOAT_EQ(after.x1, before.x1);
        EXPECT_FLOAT_EQ(after.y1, before.y1);
        EXPECT_FLOAT_EQ(after.x2, before.x2);
        EXPECT_FLOAT_EQ(after.y2, before.y2);
    }
}
