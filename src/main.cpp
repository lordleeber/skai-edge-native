#include "skai/application.hpp"
#include "skai/cli.hpp"
#include "skai/logging.hpp"
#include "skai/video/gstreamer_runtime.hpp"
#include "skai/video/rtsp_video_module.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    const auto cli = skai::parse_command_line(arguments);
    if (cli.action == skai::CliAction::Exit) {
        (cli.exit_code == 0 ? std::cout : std::cerr) << cli.message;
        return cli.exit_code;
    }

    skai::Logger logger(std::cout);

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

    skai::BoundedQueue<skai::Frame> inference_frames(2);
    skai::Application::Modules modules;
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
