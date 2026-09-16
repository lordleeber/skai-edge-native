#include "skai/gps/gps_source.hpp"

#include <gtest/gtest.h>

TEST(GpsSource, DefaultConfigReturnsFixedTaipei101Fix) {
    const skai::GpsSource source(skai::GpsConfig{});
    const auto fix = source.latest();

    ASSERT_TRUE(fix.has_value());
    EXPECT_TRUE(fix->valid);
    EXPECT_EQ(fix->source, "fixed");
    EXPECT_DOUBLE_EQ(fix->latitude, 25.033964);
    EXPECT_DOUBLE_EQ(fix->longitude, 121.564468);
    EXPECT_DOUBLE_EQ(fix->altitude_m, 10.0);
    EXPECT_GT(fix->hdop, 0.0);
    EXPECT_GT(fix->satellites_visible, 0);
    EXPECT_GT(fix->satellites_used, 0);
    EXPECT_LE(fix->satellites_used, fix->satellites_visible);
}

TEST(GpsSource, ReturnsConfiguredCoordinatesUnchanged) {
    skai::GpsConfig config;
    config.latitude = -33.868820;
    config.longitude = 151.209290;
    config.altitude_m = 58.75;

    const auto fix = skai::GpsSource(config).latest();

    ASSERT_TRUE(fix.has_value());
    EXPECT_DOUBLE_EQ(fix->latitude, config.latitude);
    EXPECT_DOUBLE_EQ(fix->longitude, config.longitude);
    EXPECT_DOUBLE_EQ(fix->altitude_m, config.altitude_m);
}

TEST(GpsSource, DisabledConfigHasNoLatestFix) {
    skai::GpsConfig config;
    config.enabled = false;
    EXPECT_FALSE(skai::GpsSource(config).latest().has_value());
}
