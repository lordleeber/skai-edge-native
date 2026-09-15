#include "skai/cli.hpp"
#include "skai/config.hpp"
#include "skai/logging.hpp"

#include <csignal>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    const auto cli = skai::parse_command_line(arguments);
    if (cli.action == skai::CliAction::Exit) {
        (cli.exit_code == 0 ? std::cout : std::cerr) << cli.message;
        return cli.exit_code;
    }

    skai::Config config;
    if (!cli.config_path.empty()) {
        const auto loaded = skai::load_config(cli.config_path);
        if (!loaded.ok) {
            skai::Logger error_logger(std::cerr);
            error_logger.log(skai::LogLevel::Error, "config", loaded.error);
            return 2;
        }
        config = loaded.config;
    }
    skai::Logger logger(std::cout, config.logging.level);
    logger.log(skai::LogLevel::Info, "config",
               cli.config_path.empty() ? "using built-in defaults" : "configuration validated");

    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0) {
        logger.log(skai::LogLevel::Error, "core", "failed to configure shutdown signals");
        return 1;
    }

    logger.log(skai::LogLevel::Info, "core", "skai-edge ready");
    int signal_number = 0;
    if (sigwait(&shutdown_signals, &signal_number) != 0) {
        logger.log(skai::LogLevel::Error, "core", "failed to wait for shutdown signal");
        return 1;
    }
    logger.log(skai::LogLevel::Info, "core", "skai-edge stopped");
    return 0;
}
