#include "skai/logging.hpp"

#include <gtest/gtest.h>

#include <regex>
#include <sstream>
#include <string>

TEST(Logging, IncludesTimestampLevelAndModule) {
    std::ostringstream output;
    skai::Logger logger(output, skai::LogLevel::Trace);
    logger.log(skai::LogLevel::Warning, "config", "invalid value");
    EXPECT_TRUE(std::regex_search(output.str(), std::regex(
        R"(timestamp="\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z")")));
    EXPECT_NE(output.str().find("level=warning"), std::string::npos);
    EXPECT_NE(output.str().find("module=config"), std::string::npos);
    EXPECT_NE(output.str().find("message=\"invalid value\""), std::string::npos);
}

TEST(Logging, FiltersAndEscapesMessages) {
    std::ostringstream output;
    skai::Logger logger(output, skai::LogLevel::Info);
    logger.log(skai::LogLevel::Debug, "core", "hidden");
    logger.log(skai::LogLevel::Error, "core", "bad\nvalue\"");
    EXPECT_EQ(output.str().find("hidden"), std::string::npos);
    EXPECT_NE(output.str().find("message=\"bad\\nvalue\\\"\""), std::string::npos);
}
