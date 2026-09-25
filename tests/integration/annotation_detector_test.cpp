#include "rtsp_test_server.hpp"
#include "skai/inference/yolo_detector.hpp"
#include "skai/inference/yolo_inference_module.hpp"
#include "skai/video/annotator.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/rtsp_source.hpp"

#include <gtest/gtest.h>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <optional>
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

TEST(AnnotationDetector, FailedInferencePassesFrameThroughAndProcessesNextFrame) {
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
    skai::YoloInferenceModule module(input, output, logger, status, api);
    skai::Config config;
    config.detector.engine = SKAI_YOLO_ENGINE;
    config.detector.annotate = true;
    ASSERT_TRUE(module.initialize(config)) << logs.str();
    ASSERT_TRUE(module.start());

    auto malformed = reference_frame();
    malformed.sequence = 1;
    malformed.bgr.resize(3); // Invalid image buffer reaches detector.run().
    ASSERT_TRUE(input.push(malformed));
    const auto fallback = output.pop_for(std::chrono::seconds(2));
    if (!fallback) {
        module.stop();
        module.wait();
        FAIL() << logs.str();
    }
    EXPECT_EQ(fallback->sequence, malformed.sequence);
    EXPECT_EQ(fallback->bgr, malformed.bgr);

    auto valid = reference_frame();
    valid.sequence = 2;
    ASSERT_TRUE(input.push(valid));
    const auto recovered = output.pop_for(std::chrono::seconds(2));
    if (!recovered) {
        module.stop();
        module.wait();
        FAIL() << logs.str();
    }
    EXPECT_EQ(recovered->sequence, valid.sequence);
    EXPECT_EQ(recovered->bgr.size(), valid.bgr.size());
    module.stop();
    module.wait();
}

TEST(AnnotationDetector, RepeatedInferenceFailuresInvalidateLatestDetections) {
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
    std::vector<std::string> published;
    events->subscribe([&](const std::string& event) { published.push_back(event); });
    skai::YoloInferenceModule module(input, output, logger, status, api, events);
    skai::Config config;
    config.detector.engine = SKAI_YOLO_ENGINE;
    ASSERT_TRUE(module.initialize(config)) << logs.str();
    ASSERT_TRUE(module.start());

    const auto deliver = [&](skai::Frame value) {
        return input.push(std::move(value)) &&
               output.pop_for(std::chrono::seconds(2)).has_value();
    };
    auto valid = reference_frame();
    valid.sequence = 1;
    if (!deliver(valid)) {
        module.stop(); module.wait(); FAIL() << logs.str();
    }
    EXPECT_TRUE(api->latest_detections().available);

    for (std::uint64_t sequence : {2ULL, 3ULL}) {
        auto malformed = reference_frame();
        malformed.sequence = sequence;
        malformed.bgr.resize(3);
        if (!deliver(std::move(malformed))) {
            module.stop(); module.wait(); FAIL() << logs.str();
        }
        EXPECT_FALSE(api->latest_detections().available);
    }
    const auto unavailable_events = std::count_if(
        published.begin(), published.end(), [](const auto& event) {
            return event.find("\"type\":\"detection\"") != std::string::npos &&
                   event.find("\"available\":false") != std::string::npos;
        });
    EXPECT_EQ(unavailable_events, 1);

    valid.sequence = 4;
    if (!deliver(std::move(valid))) {
        module.stop(); module.wait(); FAIL() << logs.str();
    }
    EXPECT_TRUE(api->latest_detections().available);
    EXPECT_EQ(api->latest_detections().frame_sequence, 4U);
    module.stop();
    module.wait();
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
    std::mutex events_mutex;
    std::condition_variable event_available;
    events->subscribe([&](const std::string& event) {
        std::lock_guard<std::mutex> lock(events_mutex);
        published_events.push_back(event);
        event_available.notify_all();
    });
    auto alerts = std::make_shared<skai::AlertManager>(nullptr, events);
    status->set_detector_expected(true);
    status->set_running(true);
    skai::YoloInferenceModule module(input, output, logger, status, api, events,
                                     alerts);
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    skai::Config config;
    config.detector.engine = SKAI_YOLO_ENGINE;
    config.detector.confidence = 0.1;
    config.detector.annotate = true;
    config.alerts.push_back({"umbrella", 0.1, 2, 0, std::nullopt});
    ASSERT_TRUE(module.initialize(config));
    ASSERT_TRUE(module.start());
    ASSERT_TRUE(input.push(reference_frame()));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    ASSERT_TRUE(input.push(reference_frame()));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    std::unique_lock<std::mutex> events_lock(events_mutex);
    ASSERT_TRUE(event_available.wait_for(events_lock, std::chrono::seconds(2), [&] {
        return std::any_of(published_events.begin(), published_events.end(),
                           [](const auto& event) {
            return event.find("\"type\":\"alert\"") != std::string::npos;
        });
    }));
    const auto alert_event = std::find_if(
        published_events.begin(), published_events.end(), [](const auto& event) {
            return event.find("\"type\":\"alert\"") != std::string::npos;
        });
    ASSERT_NE(alert_event, published_events.end());
    EXPECT_NE(alert_event->find("\"class_name\":\"umbrella\""),
              std::string::npos);
    events_lock.unlock();
    const auto alert_count = [&] {
        std::lock_guard<std::mutex> lock(events_mutex);
        return std::count_if(published_events.begin(), published_events.end(),
                             [](const auto& event) {
            return event.find("\"type\":\"alert\"") != std::string::npos;
        });
    };
    ASSERT_TRUE(input.push(reference_frame()));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    const auto alerts_before_toggle = alert_count();
    api->set_detector_enabled(false);
    ASSERT_TRUE(input.push(reference_frame()));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    api->set_detector_enabled(true);
    ASSERT_TRUE(input.push(reference_frame()));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    EXPECT_EQ(alert_count(), alerts_before_toggle);
    ASSERT_TRUE(input.push(reference_frame()));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    events_lock.lock();
    EXPECT_TRUE(event_available.wait_for(events_lock, std::chrono::seconds(2), [&] {
        return std::count_if(published_events.begin(), published_events.end(),
                             [](const auto& event) {
            return event.find("\"type\":\"alert\"") != std::string::npos;
        }) == alerts_before_toggle + 1;
    }));
    events_lock.unlock();
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
    const auto latest = api->latest_detections();
    source.stop();
    module.stop();
    module.wait();
    EXPECT_EQ(live_status.status, "running");
    EXPECT_TRUE(live_status.video_fps.has_value());
    EXPECT_TRUE(live_status.detector_fps.has_value());
    EXPECT_TRUE(live_status.last_inference_ms.has_value());
    EXPECT_TRUE(latest.available);
    EXPECT_GE(latest.frame_sequence, second->sequence);
    const auto detection_event = std::find_if(
        published_events.begin(), published_events.end(), [](const auto& event) {
            return event.find("\"type\":\"detection\"") != std::string::npos;
        });
    ASSERT_NE(detection_event, published_events.end());
    EXPECT_NE(detection_event->find("\"available\":true"), std::string::npos);
    EXPECT_NE(detection_event->find("\"detections\":"), std::string::npos);

    const auto frame = reference_frame();
    const auto original_pixels = frame.bgr;
    const auto events_before_disable = published_events.size();
    api->set_detector_enabled(false);
    ASSERT_TRUE(module.initialize(config));
    ASSERT_TRUE(module.start());
    ASSERT_TRUE(input.push(frame));
    const auto passthrough = output.pop_for(std::chrono::seconds(2));
    module.stop();
    module.wait();
    ASSERT_TRUE(passthrough.has_value()) << logs.str();
    EXPECT_EQ(passthrough->bgr, original_pixels);
    EXPECT_EQ(published_events.size(), events_before_disable);
}

TEST(AnnotationDetector, DetectionSinkReceivesCommittedResultsWithSourceIdentity) {
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
    skai::YoloInferenceModule module(input, output, logger, status, api);
    std::mutex mutex;
    std::condition_variable received;
    std::vector<std::pair<skai::Frame, std::vector<skai::DetectionDto>>> sunk;
    module.set_detection_sink([&](const skai::Frame& frame,
                                  const std::vector<skai::DetectionDto>& detections) {
        std::lock_guard<std::mutex> lock(mutex);
        skai::Frame identity;
        identity.pts_ns = frame.pts_ns;
        identity.source_generation = frame.source_generation;
        identity.width = frame.width;
        identity.height = frame.height;
        sunk.emplace_back(identity, detections);
        received.notify_all();
    });
    skai::Config config;
    config.detector.engine = SKAI_YOLO_ENGINE;
    config.detector.confidence = 0.1;
    config.detector.annotate = true;
    ASSERT_TRUE(module.initialize(config));
    ASSERT_TRUE(module.start());
    auto frame = reference_frame();
    frame.source_generation = 7;
    ASSERT_TRUE(input.push(frame));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    const auto latest = api->latest_detections();
    api->set_detector_enabled(false);
    ASSERT_TRUE(input.push(reference_frame()));
    ASSERT_TRUE(output.pop_for(std::chrono::seconds(2)).has_value()) << logs.str();
    module.stop();
    module.wait();

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(sunk.size(), 1U) << "disabled detector results must not reach the sink";
    EXPECT_EQ(sunk[0].first.pts_ns, 123U);
    EXPECT_EQ(sunk[0].first.source_generation, 7U);
    EXPECT_EQ(sunk[0].first.width, 640);
    EXPECT_EQ(sunk[0].first.height, 640);
    ASSERT_FALSE(latest.detections.empty());
    ASSERT_EQ(sunk[0].second.size(), latest.detections.size());
    for (std::size_t i = 0; i < latest.detections.size(); ++i) {
        EXPECT_EQ(sunk[0].second[i].class_name, latest.detections[i].class_name);
        EXPECT_FLOAT_EQ(sunk[0].second[i].x1, latest.detections[i].x1);
    }
}
