#include "rtsp_test_server.hpp"
#include "skai/inference/yolo_detector.hpp"
#include "skai/inference/yolo_inference_module.hpp"
#include "skai/video/annotator.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/rtsp_source.hpp"

#include <gtest/gtest.h>

#include <cuda_runtime_api.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

TEST(AnnotationDetector, RtspPipelinePublishesAnnotationsAndHonorsDisableFlag) {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        GTEST_SKIP() << "CUDA device unavailable";
    }
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::BoundedQueue<skai::Frame> input(2);
    skai::BoundedQueue<skai::Frame> output(2);
    auto status = std::make_shared<skai::RuntimeStatus>();
    auto api = std::make_shared<skai::ApiState>(status);
    auto events = std::make_shared<skai::EventChannel>();
    std::vector<std::string> published_events;
    events->subscribe([&](const std::string& event) {
        published_events.push_back(event);
    });
    status->set_detector_expected(true);
    status->set_running(true);
    skai::YoloInferenceModule module(input, output, logger, status, api, events);
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::Config config;
    config.detector.engine = SKAI_YOLO_ENGINE;
    config.detector.confidence = 0.1;
    config.detector.annotate = true;
    ASSERT_TRUE(module.initialize(config));
    ASSERT_TRUE(module.start());
    skai::RtspSource source(input, logger, skai::DecodeMode::Auto, true, status);
    skai::VideoConfig video;
    video.rtsp_url = server.url();
    video.latency_ms = 50;
    ASSERT_TRUE(source.start(video, error)) << error;
    const auto annotated = output.pop_for(std::chrono::seconds(3));
    const auto second = output.pop_for(std::chrono::seconds(3));
    ASSERT_TRUE(annotated.has_value()) << logs.str();
    ASSERT_TRUE(second.has_value()) << logs.str();
    EXPECT_GT(annotated->sequence, 0U);
    EXPECT_FALSE(annotated->bgr.empty());
    const auto live_status = status->snapshot();
    EXPECT_EQ(live_status.status, "running");
    EXPECT_TRUE(live_status.video_fps.has_value());
    EXPECT_TRUE(live_status.detector_fps.has_value());
    EXPECT_TRUE(live_status.last_inference_ms.has_value());
    const auto latest = api->latest_detections();
    EXPECT_TRUE(latest.available);
    EXPECT_GE(latest.frame_sequence, second->sequence);
    ASSERT_FALSE(published_events.empty());
    EXPECT_NE(published_events.back().find("\"type\":\"detection\""),
              std::string::npos);
    EXPECT_NE(published_events.back().find("\"detections\":"),
              std::string::npos);
    source.stop();
    module.stop();
    module.wait();

    const auto frame = reference_frame();
    const auto original_pixels = frame.bgr;
    api->set_detector_enabled(false);
    ASSERT_TRUE(module.initialize(config));
    ASSERT_TRUE(module.start());
    ASSERT_TRUE(input.push(frame));
    const auto passthrough = output.pop_for(std::chrono::seconds(2));
    module.stop();
    module.wait();
    ASSERT_TRUE(passthrough.has_value()) << logs.str();
    EXPECT_EQ(passthrough->bgr, original_pixels);
}
