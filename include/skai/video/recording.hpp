#pragma once

#include "skai/application.hpp"
#include "skai/config.hpp"
#include "skai/core/bounded_queue.hpp"
#include "skai/events.hpp"
#include "skai/logging.hpp"
#include "skai/video/recording_control.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/h264_encoder.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace skai {

class RecordingModule final : public LifecycleModule {
public:
    RecordingModule(BoundedQueue<EncodedAccessUnit>& input, Logger& logger,
                    std::shared_ptr<RecordingController> control,
                    std::shared_ptr<EventChannel> events = {});
    ~RecordingModule() override;

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;
    std::string last_error() const override;

private:
    static gchar* on_format_location(GstElement*, guint fragment_id, gpointer user_data);
    bool open_pipeline(const EncodedAccessUnit& first, std::string& error);
    void close_pipeline(bool finalize) noexcept;
    bool write_access_unit(const EncodedAccessUnit& unit, std::string& error);
    bool prepare_directory(std::string& error);
    void enforce_quota() noexcept;
    std::string next_path(unsigned int fragment_id) const;
    void publish_state();
    void fail(const std::string& error);
    void run() noexcept;

    BoundedQueue<EncodedAccessUnit>& input_;
    Logger& logger_;
    std::shared_ptr<RecordingController> control_;
    std::shared_ptr<EventChannel> events_;
    RecordingConfig config_;
    std::unique_ptr<gst::Pipeline> pipeline_;
    GstElement* appsrc_ = nullptr; // borrowed from pipeline_; worker-only
    GstElement* splitmux_ = nullptr; // borrowed from pipeline_; worker-only
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::string session_id_;
    bool waiting_for_keyframe_ = true;
    std::uint64_t expected_sequence_ = 0;
    std::uint64_t pending_access_units_ = 0;
    std::uint64_t pending_bytes_ = 0;
};

} // namespace skai
