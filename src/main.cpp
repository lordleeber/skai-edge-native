#include "skai/application.hpp"
#include "skai/cli.hpp"
#include "skai/logging.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/rtsp_video_module.hpp"
#include "skai/video/rtsp_source.hpp"
#include "skai/web/http_server.hpp"
#if SKAI_HAS_YOLO_PIPELINE
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
    skai::BoundedQueue<skai::Frame> annotated_frames(2);
    skai::Application::Modules modules;
    modules.web = std::make_unique<skai::web::HttpServer>(logger);
#if SKAI_HAS_YOLO_PIPELINE
    modules.detector = std::make_unique<skai::YoloInferenceModule>(
        inference_frames, annotated_frames, logger);
#else
    logger.log(skai::LogLevel::Warning, "detector",
               "service built without TensorRT/CUDA inference support");
#endif
    modules.video = std::make_unique<skai::RtspVideoModule>(inference_frames, logger);
    skai::Application app(cli.config_path, logger, std::move(modules));
    if (!app.initialize()) {
        skai::Logger error_logger(std::cerr);
        error_logger.log(skai::LogLevel::Error, app.last_error_module(), app.last_error());
        return 2;
    }
    if (!app.start()) {
        skai::Logger error_logger(std::cerr);
        error_logger.log(skai::LogLevel::Error, app.last_error_module(), app.last_error());
        return 1;
    }
    logger.log(skai::LogLevel::Info, "core", "skai-edge ready");
    int signal_number = 0;
    if (sigwait(&shutdown_signals, &signal_number) != 0) {
        logger.log(skai::LogLevel::Error, "core", "failed to wait for shutdown signal");
        app.stop();
        app.wait();
        return 1;
    }
    app.stop();
    app.wait();
    logger.log(skai::LogLevel::Info, "core", "skai-edge stopped");
    return 0;
}
