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

TEST(CommandLine, HelpAndVersionWorkWithRtspTest) {
    const auto help = skai::parse_command_line({"--rtsp-test", "--help"});
    EXPECT_EQ(help.action, skai::CliAction::Exit);
    EXPECT_EQ(help.exit_code, 0);
    EXPECT_NE(help.message.find("Usage:"), std::string::npos);
    const auto version = skai::parse_command_line({"--version", "--config", "site.yaml"});
    EXPECT_EQ(version.action, skai::CliAction::Exit);
    EXPECT_EQ(version.exit_code, 0);
    EXPECT_NE(version.message.find("skai-edge"), std::string::npos);
    const auto duplicate = skai::parse_command_line({"--rtsp-test", "--rtsp-test"});
    EXPECT_EQ(duplicate.exit_code, 2);
    EXPECT_NE(duplicate.message.find("--rtsp-test"), std::string::npos);
    const auto duplicate_config = skai::parse_command_line(
        {"--config", "site.yaml", "--config", "other.yaml"});
    EXPECT_EQ(duplicate_config.exit_code, 2);
    EXPECT_NE(duplicate_config.message.find("--config may be specified only once"),
              std::string::npos);
}
