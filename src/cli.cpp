#include "skai/cli.hpp"

namespace skai {
namespace {

constexpr const char* usage = "Usage: skai-edge [--help | --version]\n";

} // namespace

CliResult parse_command_line(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return {CliAction::Run, 0, {}};
    if (arguments.size() == 1 && arguments[0] == "--help") {
        return {CliAction::Exit, 0, usage};
    }
    if (arguments.size() == 1 && arguments[0] == "--version") {
        return {CliAction::Exit, 0, "skai-edge " SKAI_EDGE_VERSION "\n"};
    }
    return {CliAction::Exit, 2, "Unknown option or arguments: " + arguments[0] +
                                    "\n" + usage};
}

CliResult parse_command_line(std::initializer_list<std::string> arguments) {
    return parse_command_line(std::vector<std::string>(arguments));
}

} // namespace skai
