#include "skai/core/bounded_queue.hpp"
#include "skai/logging.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/h264_encoder.hpp"
#include "skai/video/recording.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

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
