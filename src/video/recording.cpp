#include "skai/video/recording.hpp"

#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace skai {
namespace {

constexpr std::uint64_t mib = 1024ULL * 1024ULL;

std::string json_string(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') { result += '\\'; result += character; }
        else if (character >= 0x20) result += character;
    }
    return result + '"';
}

} // namespace

void RecordingController::configure(const RecordingConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    configured_ = true;
    requested_ = config.enabled;
    status_ = {};
    status_.configured = true;
    status_.available = media_available_;
    status_.unavailable_reason = media_unavailable_reason_;
    status_.state = !media_available_ ? "unavailable" :
                    config.enabled ? "starting" : "stopped";
}

bool RecordingController::start(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) { error = "recording is not configured"; return false; }
    if (!media_available_) {
        error = media_unavailable_reason_.empty()
                    ? "source H.264 is unavailable" : media_unavailable_reason_;
        return false;
    }
    requested_ = true;
    status_.last_error.clear();
    if (!status_.active) status_.state = "starting";
    error.clear();
    return true;
}

bool RecordingController::stop(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) { error = "recording is not configured"; return false; }
    requested_ = false;
    status_.state = status_.active ? "stopping" : "stopped";
    error.clear();
    return true;
}

bool RecordingController::requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_ && media_available_;
}

RecordingStatus RecordingController::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void RecordingController::set_active(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.active = true;
    status_.state = "recording";
    status_.current_path = std::move(path);
}

void RecordingController::set_stopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.active = false;
    if (!requested_ && status_.state != "error") status_.state = "stopped";
}

void RecordingController::set_error(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    requested_ = false;
    status_.active = false;
    status_.state = "error";
    status_.last_error = std::move(error);
}

void RecordingController::commit_written(std::uint64_t access_units, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.access_units_written += access_units;
    status_.bytes_written += bytes;
}

void RecordingController::add_dropped() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++status_.access_units_dropped;
}

void RecordingController::set_media_available(bool available, std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    media_available_ = available;
    media_unavailable_reason_ = available ? std::string{} : std::move(reason);
    status_.available = available;
    status_.unavailable_reason = media_unavailable_reason_;
    if (!available) {
        status_.active = false;
        if (status_.state != "error") status_.state = "unavailable";
    } else if (requested_ && !status_.active) {
        status_.state = "starting";
    } else if (!requested_ && status_.state != "error") {
        status_.state = "stopped";
    }
}

RecordingModule::RecordingModule(BoundedQueue<EncodedAccessUnit>& input, Logger& logger,
                                 std::shared_ptr<RecordingController> control,
                                 std::shared_ptr<EventChannel> events,
                                 std::shared_ptr<RuntimeStatus> status)
    : input_(input), logger_(logger), control_(std::move(control)), events_(std::move(events)),
      status_(std::move(status)) {}

RecordingModule::~RecordingModule() { wait(); }

bool RecordingModule::initialize(const Config& config) {
    if (worker_.joinable() || !control_) return false;
    config_ = config.recording;
    control_->configure(config_);
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
    return true;
}

bool RecordingModule::start() {
    if (worker_.joinable()) return false;
    if (!gst_is_initialized()) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = "GStreamer must be initialized before recording";
        return false;
    }
    running_ = true;
    observed_queue_drops_ = input_.stats().dropped;
    worker_ = std::thread(&RecordingModule::run, this);
    return true;
}

void RecordingModule::stop() noexcept { running_ = false; }

void RecordingModule::wait() noexcept {
    stop();
    if (worker_.joinable()) worker_.join();
    if (control_) {
        std::string ignored;
        control_->stop(ignored);
    }
    close_pipeline(true);
    if (control_) control_->set_stopped();
}

std::string RecordingModule::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

bool RecordingModule::prepare_directory(std::string& error) {
    std::error_code code;
    std::filesystem::create_directories(config_.directory, code);
    if (code) { error = "could not create recording directory: " + code.message(); return false; }
    enforce_quota();
    const auto space = std::filesystem::space(config_.directory, code);
    if (code || space.available < config_.min_free_space_mb * mib) {
        error = code ? "could not inspect recording disk space: " + code.message()
                     : "insufficient free disk space for recording";
        return false;
    }
    return true;
}

bool RecordingModule::open_pipeline(const EncodedAccessUnit&, std::string& error) {
    if (!prepare_directory(error)) return false;
    pipeline_ = gst::Pipeline::create_empty(logger_, error);
    if (!pipeline_) return false;
    GstElement* source = gst_element_factory_make("appsrc", "recording_input");
    GstElement* parser = gst_element_factory_make("h264parse", "recording_parse");
    GstElement* sink = gst_element_factory_make("splitmuxsink", "recording_sink");
    if (!source || !parser || !sink) {
        if (source) gst_object_unref(source); if (parser) gst_object_unref(parser);
        if (sink) gst_object_unref(sink);
        error = "required GStreamer MP4 recording plugins are unavailable";
        close_pipeline(false);
        return false;
    }
    GstCaps* caps = gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING,
                                        "byte-stream", "alignment", G_TYPE_STRING, "au", nullptr);
    g_object_set(source, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME,
                 "block", TRUE, "max-buffers", 2ULL, "max-bytes", 0ULL,
                 "max-time", 0ULL, nullptr);
    g_object_set(parser, "config-interval", -1, nullptr);
    g_object_set(sink, "max-size-time", static_cast<guint64>(config_.segment_seconds) * GST_SECOND,
                 "muxer-factory", "mp4mux", "async-finalize", TRUE, nullptr);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipeline_->element()), source, parser, sink, nullptr);
    if (!gst_element_link(source, parser) || !gst_element_link(parser, sink)) {
        error = "could not link MP4 recording pipeline";
        close_pipeline(false);
        return false;
    }
    appsrc_ = source;
    splitmux_ = sink;
    g_signal_connect(sink, "format-location", G_CALLBACK(on_format_location), this);
    if (gst_element_set_state(pipeline_->element(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        error = "failed to start MP4 recording pipeline";
        close_pipeline(false);
        return false;
    }
    return true;
}

void RecordingModule::close_pipeline(bool finalize) noexcept {
    if (!pipeline_) return;
    bool finalized = false;
    if (finalize && appsrc_) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        while (true) {
            const auto event = pipeline_->poll(std::chrono::milliseconds(50));
            if (event.type == gst::BusEventType::Eos) { finalized = true; break; }
            if (event.type == gst::BusEventType::Error) break;
        }
    }
    if (splitmux_) g_signal_handlers_disconnect_by_data(splitmux_, this);
    appsrc_ = nullptr;
    splitmux_ = nullptr;
    pipeline_->stop();
    pipeline_.reset();
    if (finalized) control_->commit_written(pending_access_units_, pending_bytes_);
    pending_access_units_ = 0;
    pending_bytes_ = 0;
    enforce_quota();
}

bool RecordingModule::write_access_unit(const EncodedAccessUnit& unit, std::string& error) {
    guint64 queued = 0;
    g_object_get(appsrc_, "current-level-buffers", &queued, nullptr);
    if (queued >= 2) {
        control_->add_dropped();
        waiting_for_keyframe_ = true;
        return true;
    }
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, unit.bytes.size(), nullptr);
    if (!buffer) { error = "could not allocate recording buffer"; return false; }
    GstMapInfo mapped{};
    if (!gst_buffer_map(buffer, &mapped, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer); error = "could not map recording buffer"; return false;
    }
    std::memcpy(mapped.data, unit.bytes.data(), unit.bytes.size());
    gst_buffer_unmap(buffer, &mapped);
    GST_BUFFER_PTS(buffer) = unit.has_pts ? unit.pts_ns : GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buffer) = unit.has_dts ? unit.dts_ns : GST_CLOCK_TIME_NONE;
    if (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer) != GST_FLOW_OK) {
        error = "MP4 recorder rejected H.264 access unit";
        return false;
    }
    ++pending_access_units_;
    pending_bytes_ += unit.bytes.size();
    return true;
}

gchar* RecordingModule::on_format_location(GstElement*, guint fragment_id, gpointer user_data) {
    auto* self = static_cast<RecordingModule*>(user_data);
    const auto path = self->next_path(fragment_id);
    self->control_->set_active(path);
    self->publish_state();
    return g_strdup(path.c_str());
}

std::string RecordingModule::next_path(unsigned int fragment_id) const {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&time, &local);
    std::ostringstream name;
    name << "recording_" << std::put_time(&local, "%Y%m%d_%H%M%S")
         << '_' << session_id_;
    if (fragment_id) name << '_' << std::setw(3) << std::setfill('0') << fragment_id;
    name << ".mp4";
    return (std::filesystem::path(config_.directory) / name.str()).string();
}

void RecordingModule::enforce_quota() noexcept {
    std::error_code code;
    struct File { std::filesystem::path path; std::uintmax_t size; std::filesystem::file_time_type time; };
    std::vector<File> files;
    std::uintmax_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(config_.directory, code)) {
        if (code) return;
        if (!entry.is_regular_file(code) || entry.path().extension() != ".mp4" ||
            entry.path().filename().string().rfind("recording_", 0) != 0) continue;
        const auto size = entry.file_size(code);
        const auto time = entry.last_write_time(code);
        if (code) { code.clear(); continue; }
        total += size;
        files.push_back({entry.path(), size, time});
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.time < right.time;
    });
    const auto quota = config_.max_storage_mb * mib;
    for (const auto& file : files) {
        if (total <= quota) break;
        std::filesystem::remove(file.path, code);
        if (!code) total -= file.size;
    }
}

void RecordingModule::publish_state() {
    if (!events_) return;
    const auto status = control_->status();
    events_->publish(EventType::Recording,
                     "{\"state\":" + json_string(status.state) +
                     ",\"active\":" + (status.active ? "true" : "false") +
                     ",\"current_path\":" + json_string(status.current_path) +
                     ",\"last_error\":" + json_string(status.last_error) + "}");
}

void RecordingModule::fail(const std::string& error) {
    control_->set_error(error);
    { std::lock_guard<std::mutex> lock(error_mutex_); last_error_ = error; }
    logger_.log(LogLevel::Error, "recording", error);
    publish_state();
}

void RecordingModule::run() noexcept {
    while (running_) {
        if (!control_->requested()) {
            if (pipeline_) close_pipeline(true);
            waiting_for_keyframe_ = true;
            control_->set_stopped();
            publish_state();
            auto ignored = input_.pop_for(std::chrono::milliseconds(25));
            if (ignored) control_->add_dropped();
            continue;
        }
        auto unit = input_.pop_for(std::chrono::milliseconds(25));
        if (unit && status_ && unit->queued_at != std::chrono::steady_clock::time_point{}) {
            status_->observe_profile(ProfileStage::RecordingQueueWait,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - unit->queued_at).count());
        }
        const auto queue_drops = input_.stats().dropped;
        if (queue_drops != observed_queue_drops_) {
            observed_queue_drops_ = queue_drops;
            waiting_for_keyframe_ = true;
            if (pipeline_) close_pipeline(true);
        }
        if (pipeline_) {
            const auto event = pipeline_->poll(std::chrono::milliseconds(0));
            if (event.type == gst::BusEventType::Error || event.type == gst::BusEventType::Eos) {
                fail(event.type == gst::BusEventType::Error ? event.detail : "recording pipeline ended unexpectedly");
                close_pipeline(false);
                continue;
            }
        }
        if (!unit) {
            if (input_.is_shutdown()) break;
            continue;
        }
        if (unit->discontinuity) {
            waiting_for_keyframe_ = true;
            if (pipeline_) close_pipeline(true);
        }
        if (waiting_for_keyframe_) {
            if (!unit->keyframe) { control_->add_dropped(); continue; }
            waiting_for_keyframe_ = false;
            session_id_ = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        }
        std::string error;
        if (!pipeline_ && !open_pipeline(*unit, error)) {
            fail(error);
            continue;
        }
        if (!write_access_unit(*unit, error)) {
            fail(error);
            close_pipeline(false);
        }
    }
    close_pipeline(true);
}

} // namespace skai
