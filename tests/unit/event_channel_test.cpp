#include "skai/events.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(EventChannel, PublishesEverySupportedEventTypeToMultipleSubscribers) {
    skai::EventChannel channel;
    std::vector<std::string> first;
    std::vector<std::string> second;
    const auto first_id = channel.subscribe(
        [&](const std::string& event) { first.push_back(event); });
    channel.subscribe([&](const std::string& event) { second.push_back(event); });

    const std::vector<skai::EventType> types = {
        skai::EventType::Status, skai::EventType::Detection,
        skai::EventType::Gps, skai::EventType::Alert,
        skai::EventType::Recording, skai::EventType::SystemError};
    for (const auto type : types) channel.publish(type, "{\"value\":1}");

    ASSERT_EQ(first.size(), types.size());
    EXPECT_EQ(second, first);
    for (std::size_t index = 0; index < types.size(); ++index) {
        EXPECT_NE(first[index].find(std::string("\"type\":\"") +
                                    skai::event_type_name(types[index]) + "\""),
                  std::string::npos);
        EXPECT_NE(first[index].find("\"timestamp\":"), std::string::npos);
        EXPECT_NE(first[index].find("\"data\":{\"value\":1}"),
                  std::string::npos);
    }

    channel.unsubscribe(first_id);
    channel.publish(skai::EventType::Status, "{}");
    EXPECT_EQ(first.size(), types.size());
    EXPECT_EQ(second.size(), types.size() + 1);
}

TEST(EventQueue, DropsOldestPendingEventWhenClientIsSlow) {
    skai::EventQueue queue(2);
    queue.push("one");
    queue.push("two");
    queue.push("three");

    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.dropped(), 1U);
    EXPECT_EQ(queue.pop(), "two");
    EXPECT_EQ(queue.pop(), "three");
    EXPECT_FALSE(queue.pop().has_value());
}
