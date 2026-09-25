#include "skai/core/bounded_queue.hpp"
#include "skai/logging.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/h264_encoder.hpp"
#include "skai/video/recording.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

namespace {

skai::Frame frame(std::uint64_t sequence = 1) {
    skai::Frame value;
    value.sequence = sequence;
    value.timestamp = std::chrono::steady_clock::now();
    value.width = 32;
    value.height = 24;
    value.stride = value.width * 3;
    value.bgr.resize(static_cast<std::size_t>(value.stride) * value.height, 64);
    return value;
}

std::filesystem::path temporary_directory() {
    return std::filesystem::temp_directory_path() /
           ("skai-recording-test-" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

} // namespace

TEST(RecordingController, StartsAndStopsConfiguredRecording) {
    skai::RecordingController control;
    skai::RecordingConfig config;
    config.enabled = false;
    control.configure(config);
    std::string error;
    ASSERT_TRUE(control.start(error)) << error;
    EXPECT_EQ(control.status().state, "starting");
    ASSERT_TRUE(control.stop(error)) << error;
    EXPECT_EQ(control.status().state, "stopped");
}

TEST(RecordingController, KeepsDiskFailureVisibleUntilExplicitRetryOrStop) {
    skai::RecordingController control;
    skai::RecordingConfig config;
    config.enabled = true;
    control.configure(config);

    control.set_error("insufficient free disk space for recording");
    control.set_stopped(); // The idle worker still runs after a failed recording.
    control.set_media_available(false, "RTSP source is reconnecting");
    EXPECT_EQ(control.status().state, "error");
    EXPECT_FALSE(control.status().available);
    EXPECT_EQ(control.status().unavailable_reason, "RTSP source is reconnecting");
    control.set_media_available(true);
    EXPECT_EQ(control.status().state, "error");
    EXPECT_TRUE(control.status().available);
    EXPECT_FALSE(control.status().active);
    EXPECT_FALSE(control.requested());
    EXPECT_EQ(control.status().last_error,
              "insufficient free disk space for recording");

    std::string error;
    ASSERT_TRUE(control.start(error)) << error;
    EXPECT_EQ(control.status().state, "starting");
    EXPECT_TRUE(control.status().last_error.empty());
    control.set_error("disk full");
    ASSERT_TRUE(control.stop(error)) << error;
    EXPECT_EQ(control.status().state, "stopped");
}

TEST(RecordingModule, DiskSpaceFailureStaysVisibleWhileWorkerContinues) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    const auto directory = temporary_directory();
    std::filesystem::create_directories(directory);
    const auto available = std::filesystem::space(directory).available;
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::BoundedQueue<skai::EncodedAccessUnit> units(2);
    auto control = std::make_shared<skai::RecordingController>();
    skai::RecordingModule recorder(units, logger, control);
    skai::Config config;
    config.recording.directory = directory.string();
    config.recording.enabled = true;
    config.recording.min_free_space_mb = available / (1024ULL * 1024ULL) + 1024;
    ASSERT_TRUE(recorder.initialize(config));
    ASSERT_TRUE(recorder.start()) << recorder.last_error();
    skai::EncodedAccessUnit keyframe;
    keyframe.keyframe = true;
    keyframe.bytes = {0, 0, 0, 1, 0x65, 0x88};
    ASSERT_TRUE(units.push(keyframe));
    for (int attempt = 0; attempt < 100 &&
         control->status().state != "error"; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(control->status().state, "error") << logs.str();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(control->status().state, "error");
    EXPECT_EQ(control->status().last_error,
              "insufficient free disk space for recording");
    EXPECT_FALSE(control->status().active);
    recorder.wait();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(RecordingModule, FinalizesMp4FromEncodedAccessUnits) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    const auto directory = temporary_directory();
    skai::BoundedQueue<skai::Frame> frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> access_units(8);
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::H264Encoder encoder(frames, access_units, logger);
    auto control = std::make_shared<skai::RecordingController>();
    skai::RecordingModule recorder(access_units, logger, control);
    skai::Config config;
    config.recording.directory = directory.string();
    config.recording.enabled = true;
    config.recording.segment_seconds = 1;
    config.recording.max_storage_mb = 16;
    config.recording.min_free_space_mb = 0;
    ASSERT_TRUE(recorder.initialize(config));
    ASSERT_TRUE(recorder.start()) << recorder.last_error();
    ASSERT_TRUE(encoder.start({}, error)) << error;
    for (std::uint64_t sequence = 1; sequence < 16; sequence += 2) {
        ASSERT_TRUE(frames.push(frame(sequence)));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int attempt = 0; attempt < 300 && !control->status().active; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(control->status().active) << logs.str();
    frames.shutdown();
    encoder.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    recorder.wait();

    EXPECT_GT(control->status().access_units_written, 1U);

    std::uintmax_t size = 0;
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".mp4") { ++files; size += entry.file_size(); }
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    EXPECT_EQ(files, 1U);
    EXPECT_GT(size, 0U);
    EXPECT_EQ(control->status().state, "stopped");
}

TEST(RecordingModule, WaitsForKeyframeAfterStopAndRestart) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    const auto directory = temporary_directory();
    skai::BoundedQueue<skai::Frame> frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> access_units(8);
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::H264Encoder encoder(frames, access_units, logger);
    auto control = std::make_shared<skai::RecordingController>();
    skai::RecordingModule recorder(access_units, logger, control);
    skai::Config config;
    config.recording.directory = directory.string();
    config.recording.enabled = true;
    config.recording.segment_seconds = 60;
    config.recording.max_storage_mb = 16;
    config.recording.min_free_space_mb = 0;
    ASSERT_TRUE(recorder.initialize(config));
    ASSERT_TRUE(recorder.start()) << recorder.last_error();
    skai::H264EncoderConfig encoder_config;
    encoder_config.keyframe_interval = 4;
    ASSERT_TRUE(encoder.start(encoder_config, error)) << error;

    const auto push_and_wait = [&](std::uint64_t sequence) {
        const auto before = encoder.metrics().access_units_encoded;
        EXPECT_TRUE(frames.push(frame(sequence)));
        for (int attempt = 0; attempt < 200 &&
             encoder.metrics().access_units_encoded == before; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_GT(encoder.metrics().access_units_encoded, before);
    };
    const auto wait_for_state = [&](const char* state) {
        for (int attempt = 0; attempt < 200 &&
             control->status().state != state; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(control->status().state, state) << logs.str();
    };

    push_and_wait(1); // Initial IDR starts the first recording.
    wait_for_state("recording");
    ASSERT_TRUE(control->stop(error)) << error;
    wait_for_state("stopped");
    push_and_wait(2);
    push_and_wait(3); // These delta AUs are consumed while stopped.

    ASSERT_TRUE(control->start(error)) << error;
    push_and_wait(4); // Still a delta AU; it must not start a new MP4.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(control->status().active) << logs.str();
    push_and_wait(5); // key-int-max=4 produces the next IDR here.
    wait_for_state("recording");

    frames.shutdown();
    encoder.stop();
    recorder.wait();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(RecordingModule, RecoversFromEncodedQueueOverflowAtNextKeyframe) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    std::ostringstream logs;
    skai::Logger logger(logs);

    skai::BoundedQueue<skai::Frame> frames(8);
    skai::BoundedQueue<skai::EncodedAccessUnit> generated(16);
    skai::H264Encoder encoder(frames, generated, logger);
    skai::H264EncoderConfig encoder_config;
    encoder_config.keyframe_interval = 3;
    ASSERT_TRUE(encoder.start(encoder_config, error)) << error;
    for (std::uint64_t sequence = 1; sequence <= 7; ++sequence) {
        const auto before = encoder.metrics().access_units_encoded;
        ASSERT_TRUE(frames.push(frame(sequence)));
        for (int attempt = 0; attempt < 200 &&
             encoder.metrics().access_units_encoded == before; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_GT(encoder.metrics().access_units_encoded, before);
    }
    frames.shutdown();
    encoder.stop();

    std::vector<skai::EncodedAccessUnit> units;
    while (auto unit = generated.pop_for(std::chrono::milliseconds(0))) {
        units.push_back(std::move(*unit));
    }
    ASSERT_GE(units.size(), 4U);
    const auto first_keyframe = std::find_if(units.begin(), units.end(),
        [](const auto& unit) { return unit.keyframe; });
    ASSERT_NE(first_keyframe, units.end());
    const auto delta = std::find_if(std::next(first_keyframe), units.end(),
        [](const auto& unit) { return !unit.keyframe; });
    ASSERT_NE(delta, units.end());
    const auto next_keyframe = std::find_if(std::next(delta), units.end(),
        [](const auto& unit) { return unit.keyframe; });
    ASSERT_NE(next_keyframe, units.end());

    const auto directory = temporary_directory();
    skai::BoundedQueue<skai::EncodedAccessUnit> recording_units(1);
    auto control = std::make_shared<skai::RecordingController>();
    skai::RecordingModule recorder(recording_units, logger, control);
    skai::Config config;
    config.recording.directory = directory.string();
    config.recording.enabled = true;
    config.recording.segment_seconds = 60;
    config.recording.max_storage_mb = 16;
    config.recording.min_free_space_mb = 0;
    ASSERT_TRUE(recorder.initialize(config));
    ASSERT_TRUE(recorder.start()) << recorder.last_error();
    ASSERT_TRUE(recording_units.push(*first_keyframe));
    for (int attempt = 0; attempt < 200 && !control->status().active; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(control->status().active) << logs.str();
    const auto first_path = control->status().current_path;

    for (int attempt = 0; attempt < 1000; ++attempt) {
        recording_units.push(*delta);
    }
    ASSERT_GT(recording_units.stats().dropped, 0U);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    recording_units.push(*delta);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(control->status().current_path, first_path);

    ASSERT_TRUE(recording_units.push(*next_keyframe));
    for (int attempt = 0; attempt < 200 &&
         control->status().current_path == first_path; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_NE(control->status().current_path, first_path) << logs.str();

    recording_units.shutdown();
    recorder.wait();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
