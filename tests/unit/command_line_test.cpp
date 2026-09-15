#include "skai/cli.hpp"

#include <gtest/gtest.h>

TEST(CommandLine, NoArgumentsStartService) {
    const auto result = skai::parse_command_line({});
    EXPECT_EQ(result.action, skai::CliAction::Run);
    EXPECT_EQ(result.exit_code, 0);
}

TEST(CommandLine, HelpPrintsUsage) {
    const auto result = skai::parse_command_line({"--help"});
    EXPECT_EQ(result.action, skai::CliAction::Exit);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("Usage:"), std::string::npos);
}

TEST(CommandLine, VersionPrintsVersion) {
    const auto result = skai::parse_command_line({"--version"});
    EXPECT_EQ(result.action, skai::CliAction::Exit);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("skai-edge"), std::string::npos);
    EXPECT_NE(result.message.find(SKAI_EDGE_VERSION), std::string::npos);
}

TEST(CommandLine, UnknownOptionFailsWithUsage) {
    const auto result = skai::parse_command_line({"--unknown"});
    EXPECT_EQ(result.action, skai::CliAction::Exit);
    EXPECT_NE(result.exit_code, 0);
    EXPECT_NE(result.message.find("Usage:"), std::string::npos);
    EXPECT_NE(result.message.find("--unknown"), std::string::npos);
}
