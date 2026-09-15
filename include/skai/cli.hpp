#pragma once

#include <initializer_list>
#include <string>
#include <vector>

namespace skai {

enum class CliAction { Run, Exit };

struct CliResult {
    CliAction action;
    int exit_code;
    std::string message;
};

CliResult parse_command_line(const std::vector<std::string>& arguments);
CliResult parse_command_line(std::initializer_list<std::string> arguments);

} // namespace skai
