#include "skai/core/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

TEST(BoundedQueue, RequiresPositiveFixedCapacity) {
    EXPECT_THROW((skai::BoundedQueue<int>(0)), std::invalid_argument);
    skai::BoundedQueue<int> queue(2);
    EXPECT_EQ(queue.capacity(), 2U);
}

TEST(BoundedQueue, DropsOldestWhenFullAndTracksStatistics) {
    skai::BoundedQueue<int> queue(2);
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));
    ASSERT_TRUE(queue.push(3));

    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.pop_for(std::chrono::milliseconds(1)), 2);
    EXPECT_EQ(queue.pop_for(std::chrono::milliseconds(1)), 3);
    const auto stats = queue.stats();
    EXPECT_EQ(stats.pushed, 3U);
    EXPECT_EQ(stats.popped, 2U);
    EXPECT_EQ(stats.dropped, 1U);
    EXPECT_EQ(stats.high_water_mark, 2U);
}

TEST(BoundedQueue, BlockingPopWaitsForProducer) {
    skai::BoundedQueue<int> queue(1);
    auto result = std::async(std::launch::async, [&queue] { return queue.pop(); });
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    ASSERT_TRUE(queue.push(42));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(result.get(), 42);
}

TEST(BoundedQueue, TimedPopReturnsWhenQueueIsEmpty) {
    skai::BoundedQueue<int> queue(1);
    EXPECT_FALSE(queue.pop_for(std::chrono::milliseconds(20)).has_value());
    EXPECT_EQ(queue.stats().popped, 0U);
}

TEST(BoundedQueue, ShutdownWakesBlockedConsumer) {
    skai::BoundedQueue<int> queue(1);
    auto result = std::async(std::launch::async, [&queue] { return queue.pop(); });
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    queue.shutdown();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(result.get().has_value());
    EXPECT_TRUE(queue.is_shutdown());
}

TEST(BoundedQueue, ShutdownDrainsBufferedItemsAndRejectsNewPushes) {
    skai::BoundedQueue<int> queue(2);
    ASSERT_TRUE(queue.push(7));
    ASSERT_TRUE(queue.push(8));
    queue.shutdown();
    queue.shutdown();
    EXPECT_FALSE(queue.push(9));
    EXPECT_EQ(queue.pop(), 7);
    EXPECT_EQ(queue.pop(), 8);
    EXPECT_FALSE(queue.pop().has_value());
    EXPECT_EQ(queue.stats().pushed, 2U);
    EXPECT_EQ(queue.stats().popped, 2U);
}

TEST(BoundedQueue, CarriesMoveOnlyValues) {
    skai::BoundedQueue<std::unique_ptr<int>> queue(1);
    ASSERT_TRUE(queue.push(std::make_unique<int>(7)));
    auto item = queue.pop_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(**item, 7);
}

TEST(BoundedQueue, ConcurrentProducersAndConsumersPreserveValues) {
    constexpr int producer_count = 4;
    constexpr int values_per_producer = 200;
    constexpr int total_values = producer_count * values_per_producer;
    skai::BoundedQueue<int> queue(total_values);
    std::vector<int> consumed;
    std::mutex consumed_mutex;
    std::atomic<bool> read_stats{true};
    std::atomic<bool> stats_valid{true};

    std::thread observer([&] {
        while (read_stats) {
            const auto snapshot = queue.stats();
            if (snapshot.high_water_mark > queue.capacity() || queue.size() > queue.capacity()) {
                stats_valid = false;
            }
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> consumers;
    for (int index = 0; index < 2; ++index) {
        consumers.emplace_back([&] {
            while (auto value = queue.pop()) {
                std::lock_guard<std::mutex> lock(consumed_mutex);
                consumed.push_back(*value);
            }
        });
    }
    std::vector<std::thread> producers;
    for (int index = 0; index < producer_count; ++index) {
        producers.emplace_back([&, index] {
            for (int value = 0; value < values_per_producer; ++value) {
                queue.push(index * values_per_producer + value);
            }
        });
    }
    for (auto& producer : producers) producer.join();
    queue.shutdown();
    for (auto& consumer : consumers) consumer.join();
    read_stats = false;
    observer.join();

    std::sort(consumed.begin(), consumed.end());
    std::vector<int> expected(total_values);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(consumed, expected);
    EXPECT_TRUE(stats_valid);
    const auto stats = queue.stats();
    EXPECT_EQ(stats.pushed, static_cast<std::size_t>(total_values));
    EXPECT_EQ(stats.popped, static_cast<std::size_t>(total_values));
    EXPECT_EQ(stats.dropped, 0U);
    EXPECT_LE(stats.high_water_mark, queue.capacity());
}
