#include "skai/cli.hpp"

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

    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0) {
        std::cerr << "Failed to configure shutdown signals\n";
        return 1;
    }

    std::cout << "skai-edge ready\n" << std::flush;
    int signal_number = 0;
    if (sigwait(&shutdown_signals, &signal_number) != 0) {
        std::cerr << "Failed to wait for shutdown signal\n";
        return 1;
    }
    std::cout << "skai-edge stopped\n";
    return 0;
}
