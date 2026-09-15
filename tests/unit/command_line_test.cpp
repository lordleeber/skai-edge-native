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

TEST(CommandLine, ConfigOptionStartsServiceWithPath) {
    const auto result = skai::parse_command_line({"--config", "config/site.yaml"});
    EXPECT_EQ(result.action, skai::CliAction::Run);
    EXPECT_EQ(result.config_path, "config/site.yaml");
}

TEST(CommandLine, ConfigOptionRequiresPath) {
    const auto result = skai::parse_command_line({"--config"});
    EXPECT_EQ(result.action, skai::CliAction::Exit);
    EXPECT_NE(result.exit_code, 0);
    EXPECT_NE(result.message.find("--config"), std::string::npos);
}

TEST(CommandLine, RtspTestAcceptsConfigInEitherOrder) {
    for (const auto& args : {std::vector<std::string>{"--rtsp-test", "--config", "config/site.yaml"},
                             std::vector<std::string>{"--config", "config/site.yaml", "--rtsp-test"}}) {
        const auto result = skai::parse_command_line(args);
        EXPECT_EQ(result.action, skai::CliAction::Run);
        EXPECT_TRUE(result.rtsp_test);
        EXPECT_EQ(result.config_path, "config/site.yaml");
    }
    EXPECT_NE(skai::parse_command_line({"--help"}).message.find("--rtsp-test"),
              std::string::npos);
}
