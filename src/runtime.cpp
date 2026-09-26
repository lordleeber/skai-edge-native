#include "skai/application.hpp"
#include "skai/runtime.hpp"
#include "skai/events.hpp"
#include "skai/gps/gps_module.hpp"
#include "skai/gps/gps_state.hpp"
#include "skai/health.hpp"
#include "skai/logging.hpp"
#include "skai/storage/database.hpp"
#include "skai/storage/alert_repository.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/rtsp_video_module.hpp"
#include "skai/video/rtsp_source.hpp"
#include "skai/video/recording.hpp"
#include "skai/web/http_server.hpp"
#include "skai/webrtc/ice_runtime_module.hpp"
#include "skai/webrtc/webrtc_manager.hpp"
#include "skai/webrtc/whip_publisher.hpp"
#include "skai/alerts/alert_manager.hpp"
#include "skai/inference/yolo_inference_module.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct skai::Runtime::State {
    skai::BoundedQueue<skai::Frame> inference_frames{2};
    skai::BoundedQueue<skai::EncodedAccessUnit> encoded_access_units{120};
    std::shared_ptr<skai::RuntimeStatus> runtime_status;
    std::shared_ptr<skai::ApiState> api_state;
    std::shared_ptr<skai::GpsState> gps_state;
    std::shared_ptr<skai::RecordingController> recording_control;
    std::shared_ptr<skai::WebRtcManager> webrtc_manager;
    std::shared_ptr<skai::AlertRepository> alert_repository;
    std::shared_ptr<skai::HealthWatchdog> watchdog;
    skai::RtspVideoModule* video_source = nullptr;
    skai::Database* database_state = nullptr;
    skai::web::HttpServer* web_state = nullptr;
    skai::WhipPublisher* whip_sink = nullptr;
    std::unique_ptr<skai::Application> app;
};

skai::Runtime::Runtime(std::string config_path, skai::Logger& logger,
        std::shared_ptr<skai::EventChannel> events, skai::InferenceBackendFactory backend_factory)
    : state_(std::make_unique<State>()) {
    if (!events) events = std::make_shared<skai::EventChannel>();
    auto& inference_frames = state_->inference_frames;
    auto& encoded_access_units = state_->encoded_access_units;
    auto runtime_status = std::make_shared<skai::RuntimeStatus>();
    auto gps_state = std::make_shared<skai::GpsState>();
    auto api_state = std::make_shared<skai::ApiState>(runtime_status, gps_state);
    skai::Application::Modules modules;
    auto database = std::make_unique<skai::Database>();
    auto* database_state = database.get();
    auto alert_repository = std::make_shared<skai::AlertRepository>(*database);
    auto recording_control = std::make_shared<skai::RecordingController>();
    auto webrtc_manager = std::make_shared<skai::WebRtcManager>(logger);
    auto watchdog = std::make_shared<skai::HealthWatchdog>();
    auto whip_publisher = std::make_unique<skai::WhipPublisher>(logger);
    auto* whip_sink = whip_publisher.get();
    recording_control->set_media_available(
        false, "waiting for a browser-compatible H.264 source");
    webrtc_manager->set_media_available(
        false, "waiting for a browser-compatible H.264 source");
    modules.storage = std::move(database);
    modules.webrtc = std::make_unique<skai::IceRuntimeModule>(logger);
    modules.whip = std::move(whip_publisher);
    auto& video_source = state_->video_source;
    const auto runtime_metrics = [state = state_.get(), whip_sink] {
        skai::MetricsSnapshot metrics;
        if (state->video_source) metrics.rtsp = state->video_source->diagnostics();
        metrics.inference_queue = state->inference_frames.stats();
        metrics.inference_queue_depth = state->inference_frames.size();
        metrics.encoded_queue = state->encoded_access_units.stats();
        metrics.encoded_queue_depth = state->encoded_access_units.size();
        metrics.whip = whip_sink->metrics();
        return metrics;
    };
    auto web_server = std::make_unique<skai::web::HttpServer>(
        logger, runtime_status, api_state, events, alert_repository,
        recording_control, webrtc_manager, runtime_metrics,
        [watchdog] { return watchdog->snapshot(); });
    auto* web_state = web_server.get();
    modules.web = std::move(web_server);
    modules.recording = std::make_unique<skai::RecordingModule>(
        encoded_access_units, logger, recording_control, events, runtime_status);
    if (backend_factory || skai::inference_backend_available()) {
        auto alert_manager = std::make_shared<skai::AlertManager>(
            gps_state, events, alert_repository, &logger);
        auto detector = std::make_unique<skai::YoloInferenceModule>(
            inference_frames, logger, runtime_status, api_state, events, alert_manager,
            backend_factory ? std::move(backend_factory) : skai::make_inference_backend);
        detector->set_detection_sink([whip_sink](const skai::Frame& frame,
                                                 const std::vector<skai::DetectionDto>& detections) {
            skai::SeiSourceResult result{frame.pts_ns, frame.source_generation, {}};
            result.boxes.reserve(detections.size());
            for (const auto& detection : detections) {
                result.boxes.push_back(skai::normalize_sei_box(
                    detection.x1, detection.y1, detection.x2, detection.y2, frame.width,
                    frame.height, detection.class_name, detection.confidence));
            }
            whip_sink->publish_detections(std::move(result));
        });
        modules.detector = std::move(detector);
    } else {
        api_state->set_detector_supported(false);
        logger.log(skai::LogLevel::Warning, "detector",
                   "service built without TensorRT/CUDA inference support");
    }
    auto video_module = std::make_unique<skai::RtspVideoModule>(
        inference_frames, encoded_access_units, logger, runtime_status,
        [webrtc_manager, whip_sink](const skai::EncodedAccessUnit& unit) {
            webrtc_manager->publish_access_unit(unit);
            whip_sink->publish_access_unit(unit);
        },
        [recording_control, webrtc_manager](bool available,
                                             const std::string& reason) {
            recording_control->set_media_available(available, reason);
            webrtc_manager->set_media_available(available, reason);
        });
    video_source = video_module.get();
    modules.video = std::move(video_module);
    modules.gps = std::make_unique<skai::GpsModule>(gps_state);
    state_->app = std::make_unique<skai::Application>(config_path, logger, std::move(modules));
    state_->runtime_status = runtime_status;
    state_->api_state = api_state;
    state_->gps_state = gps_state;
    state_->recording_control = recording_control;
    state_->webrtc_manager = webrtc_manager;
    state_->alert_repository = alert_repository;
    state_->watchdog = watchdog;
    state_->web_state = web_state;
    state_->database_state = database_state;
    state_->whip_sink = whip_sink;
}

skai::Runtime::~Runtime() { stop(); wait(); }

bool skai::Runtime::initialize() {
    auto& app = *state_->app;
    state_->runtime_status->reset_profiling();
    if (!app.initialize()) return false;
    const auto runtime_status = state_->runtime_status;
    const auto api_state = state_->api_state;
    const auto gps_state = state_->gps_state;
    const auto recording_control = state_->recording_control;
    const auto webrtc_manager = state_->webrtc_manager;
    const auto watchdog = state_->watchdog;
    auto* video_source = state_->video_source;
    auto* database_state = state_->database_state;
    auto* web_state = state_->web_state;
    auto* whip_sink = state_->whip_sink;
    const auto health_config = app.config();
    using skai::HealthComponent;
    watchdog->set_probe(HealthComponent::VideoSource, [video_source, health_config] {
        return skai::video_health(video_source->diagnostics(), health_config.video.stall_timeout_ms);
    });
    watchdog->set_probe(HealthComponent::Detector, [api_state, runtime_status] {
        return skai::detector_health(api_state->detector_supported(),
                                     api_state->detector_enabled(), runtime_status->snapshot());
    });
    watchdog->set_probe(HealthComponent::Encoder, [runtime_status, webrtc_manager, health_config] {
        return skai::encoder_health(runtime_status->snapshot(),
                                    webrtc_manager->diagnostics().media_available,
                                    health_config.recording.enabled || health_config.webrtc.enabled ||
                                    health_config.whip.enabled);
    });
    watchdog->set_probe(HealthComponent::Recorder, [recording_control, health_config] {
        return skai::recorder_health(recording_control->status(), recording_control->requested(),
                                     health_config.recording.enabled);
    });
    watchdog->set_probe(HealthComponent::Gps, [gps_state, health_config] {
        return skai::gps_health(health_config.gps.enabled, gps_state->latest());
    });
    watchdog->set_probe(HealthComponent::Web, [web_state] {
        return skai::web_health(web_state->serving());
    }, true);
    watchdog->set_probe(HealthComponent::WebRtc, [webrtc_manager, whip_sink] {
        return skai::webrtc_health(webrtc_manager->diagnostics(), whip_sink->metrics());
    });
    watchdog->set_probe(HealthComponent::Database, [database_state] {
        return skai::database_health(database_state->health_error());
    }, true);
    return true;
}

bool skai::Runtime::start() {
    if (!state_->app->start()) return false;
    state_->runtime_status->set_running(true);
    state_->watchdog->start();
    return true;
}

void skai::Runtime::stop() noexcept {
    state_->runtime_status->set_running(false);
    state_->watchdog->stop();
    state_->app->stop();
}
void skai::Runtime::wait() noexcept { state_->app->wait(); }
skai::Config skai::Runtime::config() const { return state_->app->config(); }
std::string skai::Runtime::last_error() const { return state_->app->last_error(); }
std::string skai::Runtime::last_error_module() const { return state_->app->last_error_module(); }
unsigned short skai::Runtime::http_port() const { return state_->web_state->port(); }
std::shared_ptr<skai::RuntimeStatus> skai::Runtime::status() const { return state_->runtime_status; }
std::shared_ptr<skai::ApiState> skai::Runtime::api_state() const { return state_->api_state; }
std::shared_ptr<skai::RecordingController> skai::Runtime::recording_control() const { return state_->recording_control; }
std::shared_ptr<skai::WebRtcManager> skai::Runtime::webrtc_manager() const { return state_->webrtc_manager; }
std::shared_ptr<skai::AlertRepository> skai::Runtime::alert_repository() const { return state_->alert_repository; }

std::unique_ptr<skai::Runtime> skai::create_runtime(std::string config_path,
        skai::Logger& logger, std::shared_ptr<skai::EventChannel> events,
        skai::InferenceBackendFactory backend_factory) {
    return std::make_unique<skai::Runtime>(std::move(config_path), logger,
                                         std::move(events), std::move(backend_factory));
}
