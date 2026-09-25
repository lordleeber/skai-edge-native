#include "skai/application.hpp"
#include "skai/cli.hpp"
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
#if SKAI_HAS_YOLO_PIPELINE
#include "skai/alerts/alert_manager.hpp"
#include "skai/inference/yolo_inference_module.hpp"
#endif

#include <csignal>
#include <cerrno>
#include <iostream>
#include <memory>
#include <string>
#include <ctime>
#include <utility>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    const auto cli = skai::parse_command_line(arguments);
    if (cli.action == skai::CliAction::Exit) {
        (cli.exit_code == 0 ? std::cout : std::cerr) << cli.message;
        return cli.exit_code;
    }

    skai::Logger logger(cli.rtsp_test ? std::cerr : std::cout);
    auto events = std::make_shared<skai::EventChannel>();
    logger.set_error_sink([events](const std::string& module,
                                   const std::string& message) {
        events->publish(skai::EventType::SystemError,
                        skai::make_system_error_data(module, message));
    });

    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0) {
        logger.log(skai::LogLevel::Error, "core", "failed to configure shutdown signals");
        return 1;
    }

    std::string gst_error;
    if (!skai::gst::initialize_once(gst_error)) {
        skai::Logger error_logger(std::cerr);
        error_logger.log(skai::LogLevel::Error, "gstreamer", gst_error);
        return 1;
    }

    if (cli.rtsp_test) {
        const auto config = cli.config_path.empty()
                                ? skai::ConfigResult{true, skai::Config{}, {}}
                                : skai::load_config(cli.config_path);
        if (!config.ok) {
            logger.log(skai::LogLevel::Error, "config", config.error);
            return 2;
        }
        skai::BoundedQueue<skai::Frame> diagnostic_frames(2);
        skai::RtspSource source(diagnostic_frames, logger, skai::DecodeMode::Auto, false);
        std::string error;
        if (!source.start(config.config.video, error)) {
            logger.log(skai::LogLevel::Error, "video", error);
            return 2;
        }
        logger.log(skai::LogLevel::Info, "video", "rtsp-test ready");
        for (;;) {
            std::cout << skai::serialize_rtsp_metrics(source.diagnostics()) << std::endl;
            timespec timeout{1, 0};
            const int signal_number = sigtimedwait(&shutdown_signals, nullptr, &timeout);
            if (signal_number == SIGINT || signal_number == SIGTERM) break;
            if (signal_number < 0 && errno != EAGAIN && errno != EINTR) {
                logger.log(skai::LogLevel::Error, "core", "failed to wait for shutdown signal");
                source.stop();
                return 1;
            }
        }
        source.stop();
        logger.log(skai::LogLevel::Info, "video", "rtsp-test stopped");
        return 0;
    }

    skai::BoundedQueue<skai::Frame> inference_frames(2);
    skai::BoundedQueue<skai::EncodedAccessUnit> encoded_access_units(120);
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
    skai::RtspVideoModule* video_source = nullptr;
    const auto runtime_metrics = [&inference_frames, &encoded_access_units,
                                  &video_source, whip_sink] {
        skai::MetricsSnapshot metrics;
        if (video_source) metrics.rtsp = video_source->diagnostics();
        metrics.inference_queue = inference_frames.stats();
        metrics.inference_queue_depth = inference_frames.size();
        metrics.encoded_queue = encoded_access_units.stats();
        metrics.encoded_queue_depth = encoded_access_units.size();
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
        encoded_access_units, logger, recording_control, events);
#if SKAI_HAS_YOLO_PIPELINE
    auto alert_manager = std::make_shared<skai::AlertManager>(
        gps_state, events, alert_repository, &logger);
    auto detector = std::make_unique<skai::YoloInferenceModule>(
        inference_frames, logger, runtime_status, api_state, events, alert_manager);
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
#else
    api_state->set_detector_supported(false);
    logger.log(skai::LogLevel::Warning, "detector",
               "service built without TensorRT/CUDA inference support");
#endif
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
    skai::Application app(cli.config_path, logger, std::move(modules));
    if (!app.initialize()) {
        skai::Logger error_logger(std::cerr);
        error_logger.log(skai::LogLevel::Error, app.last_error_module(), app.last_error());
        return 2;
    }
    const auto health_config = app.config();
    using skai::HealthComponent;
    watchdog->set_probe(HealthComponent::VideoSource, [video_source, health_config] {
        return skai::video_health(video_source->diagnostics(), health_config.video.stall_timeout_ms);
    });
    watchdog->set_probe(HealthComponent::Detector, [api_state, runtime_status] {
        return skai::detector_health(api_state->detector_supported(),
                                     api_state->detector_enabled(), runtime_status->snapshot());
    });
    watchdog->set_probe(HealthComponent::Encoder, [runtime_status, webrtc_manager] {
        return skai::encoder_health(runtime_status->snapshot(),
                                    webrtc_manager->diagnostics().media_available);
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
    watchdog->set_probe(HealthComponent::WebRtc, [webrtc_manager] {
        return skai::webrtc_health(webrtc_manager->diagnostics());
    });
    watchdog->set_probe(HealthComponent::Database, [database_state] {
        return skai::database_health(database_state->is_open());
    }, true);
    if (!app.start()) {
        skai::Logger error_logger(std::cerr);
        error_logger.log(skai::LogLevel::Error, app.last_error_module(), app.last_error());
        return 1;
    }
    runtime_status->set_running(true);
    watchdog->start();
    logger.log(skai::LogLevel::Info, "core", "skai-edge ready");
    int signal_number = 0;
    if (sigwait(&shutdown_signals, &signal_number) != 0) {
        logger.log(skai::LogLevel::Error, "core", "failed to wait for shutdown signal");
        runtime_status->set_running(false);
        watchdog->stop();
        app.stop();
        app.wait();
        return 1;
    }
    runtime_status->set_running(false);
    watchdog->stop();
    app.stop();
    app.wait();
    logger.log(skai::LogLevel::Info, "core", "skai-edge stopped");
    return 0;
}
