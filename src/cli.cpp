#include "skai/cli.hpp"

#include <algorithm>

namespace skai {
namespace {

constexpr const char* usage = "Usage: skai-edge [--help | --version | --rtsp-test] [--config PATH]\n";

} // namespace

CliResult parse_command_line(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return {CliAction::Run, 0, {}};
    if (std::find(arguments.begin(), arguments.end(), "--help") != arguments.end()) {
        return {CliAction::Exit, 0, usage};
    }
    if (std::find(arguments.begin(), arguments.end(), "--version") != arguments.end()) {
        return {CliAction::Exit, 0, "skai-edge " SKAI_EDGE_VERSION "\n"};
    }
    CliResult result{CliAction::Run, 0, {}};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--rtsp-test" && !result.rtsp_test) {
            result.rtsp_test = true;
        } else if (arguments[index] == "--rtsp-test") {
            return {CliAction::Exit, 2, "--rtsp-test may be specified only once\n" +
                                        std::string(usage)};
        } else if (arguments[index] == "--config" && result.config_path.empty()) {
            if (++index == arguments.size() || arguments[index].empty() ||
                arguments[index][0] == '-') {
                return {CliAction::Exit, 2, "--config requires a file path\n" +
                                            std::string(usage)};
            }
            result.config_path = arguments[index];
        } else if (arguments[index] == "--config") {
            return {CliAction::Exit, 2, "--config may be specified only once\n" +
                                        std::string(usage)};
        } else {
            return {CliAction::Exit, 2, "Unknown option or arguments: " + arguments[index] +
                                        "\n" + usage};
        }
    }
    return result;
}

CliResult parse_command_line(std::initializer_list<std::string> arguments) {
    return parse_command_line(std::vector<std::string>(arguments));
}

} // namespace skai
