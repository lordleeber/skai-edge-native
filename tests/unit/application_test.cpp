#include "skai/application.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

class RecordingModule final : public skai::LifecycleModule {
public:
    RecordingModule(std::string name, std::vector<std::string>& events,
                    std::atomic<int>& joins, bool fail_initialize = false,
                    bool fail_start = false)
        : name_(std::move(name)), events_(events), joins_(joins),
          fail_initialize_(fail_initialize), fail_start_(fail_start) {}

    bool initialize(const skai::Config&) override {
        events_.push_back(name_ + ".initialize");
        return !fail_initialize_;
    }

    bool start() override {
        events_.push_back(name_ + ".start");
        if (fail_start_) return false;
        stopped_ = false;
        worker_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopped_; });
        });
        return true;
    }

    void stop() noexcept override {
        events_.push_back(name_ + ".stop");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        ready_.notify_all();
    }

    void wait() noexcept override {
        events_.push_back(name_ + ".wait");
        if (worker_.joinable()) {
            worker_.join();
            ++joins_;
        }
    }

private:
    std::string name_;
    std::vector<std::string>& events_;
    std::atomic<int>& joins_;
    bool fail_initialize_;
    bool fail_start_;
    bool stopped_ = false;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread worker_;
};

skai::Application::Modules modules(std::vector<std::string>& events,
                                   std::atomic<int>& joins,
                                   const std::string& failing_init = {},
                                   const std::string& failing_start = {}) {
    skai::Application::Modules result;
    result.web = std::make_unique<RecordingModule>("web", events, joins,
                                                    failing_init == "web", failing_start == "web");
    result.detector = std::make_unique<RecordingModule>("detector", events, joins,
                                                         failing_init == "detector", failing_start == "detector");
    result.video = std::make_unique<RecordingModule>("video", events, joins,
                                                      failing_init == "video", failing_start == "video");
    result.gps = std::make_unique<RecordingModule>("gps", events, joins,
                                                    failing_init == "gps", failing_start == "gps");
    return result;
}

} // namespace

TEST(Application, StartsInDependencyOrderAndStopsInReverseOrder) {
    std::vector<std::string> events;
    std::atomic<int> joins{0};
    std::ostringstream output;
    skai::Logger logger(output);
    skai::Application app({}, logger, modules(events, joins));

    ASSERT_TRUE(app.initialize()) << app.last_error();
    ASSERT_TRUE(app.start()) << app.last_error();
    app.stop();
    app.wait();

    EXPECT_EQ(events, (std::vector<std::string>{
        "web.initialize", "detector.initialize", "video.initialize", "gps.initialize",
        "web.start", "detector.start", "video.start", "gps.start",
        "gps.stop", "video.stop", "detector.stop", "web.stop",
        "gps.wait", "video.wait", "detector.wait", "web.wait"}));
    EXPECT_EQ(joins, 4);
}

TEST(Application, RepeatedStartStopJoinsAllWorkers) {
    std::vector<std::string> events;
    std::atomic<int> joins{0};
    std::ostringstream output;
    skai::Logger logger(output);
    skai::Application app({}, logger, modules(events, joins));

    for (int cycle = 0; cycle < 20; ++cycle) {
        ASSERT_TRUE(app.initialize()) << app.last_error();
        ASSERT_TRUE(app.start()) << app.last_error();
        app.stop();
        app.stop();
        app.wait();
        app.wait();
    }
    EXPECT_EQ(joins, 80);
}

TEST(Application, FailedInitializationCleansUpInitializedModules) {
    std::vector<std::string> events;
    std::atomic<int> joins{0};
    std::ostringstream output;
    skai::Logger logger(output);
    skai::Application app({}, logger, modules(events, joins, "video"));

    EXPECT_FALSE(app.initialize());
    EXPECT_NE(app.last_error().find("video"), std::string::npos);
    EXPECT_EQ(app.last_error_module(), "video");
    EXPECT_EQ(events, (std::vector<std::string>{
        "web.initialize", "detector.initialize", "video.initialize",
        "video.stop", "detector.stop", "web.stop",
        "video.wait", "detector.wait", "web.wait"}));
    EXPECT_EQ(joins, 0);
}

TEST(Application, FailedStartStopsAndJoinsAllInitializedModules) {
    std::vector<std::string> events;
    std::atomic<int> joins{0};
    std::ostringstream output;
    skai::Logger logger(output);
    skai::Application app({}, logger, modules(events, joins, {}, "video"));

    ASSERT_TRUE(app.initialize());
    EXPECT_FALSE(app.start());
    EXPECT_NE(app.last_error().find("video"), std::string::npos);
    EXPECT_EQ(app.last_error_module(), "video");
    EXPECT_EQ(events, (std::vector<std::string>{
        "web.initialize", "detector.initialize", "video.initialize", "gps.initialize",
        "web.start", "detector.start", "video.start",
        "gps.stop", "video.stop", "detector.stop", "web.stop",
        "gps.wait", "video.wait", "detector.wait", "web.wait"}));
    EXPECT_EQ(joins, 2);
}

TEST(Application, InvalidConfigPreventsModuleInitialization) {
    std::vector<std::string> events;
    std::atomic<int> joins{0};
    std::ostringstream output;
    skai::Logger logger(output);
    skai::Application app(SKAI_INVALID_CONFIG, logger, modules(events, joins));

    EXPECT_FALSE(app.initialize());
    EXPECT_NE(app.last_error().find("web.port"), std::string::npos);
    EXPECT_EQ(app.last_error_module(), "config");
    EXPECT_TRUE(events.empty());
}

TEST(Application, WaitBlocksUntilStop) {
    std::vector<std::string> events;
    std::atomic<int> joins{0};
    std::ostringstream output;
    skai::Logger logger(output);
    {
        skai::Application app({}, logger, modules(events, joins));
        ASSERT_TRUE(app.initialize());
        ASSERT_TRUE(app.start());
        auto waiting = std::async(std::launch::async, [&app] { app.wait(); });
        EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
        app.stop();
        EXPECT_EQ(waiting.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    }
    EXPECT_EQ(joins, 4);
}

TEST(Application, DestructorStopsAndJoinsActiveWorkers) {
    std::vector<std::string> events;
    std::atomic<int> joins{0};
    std::ostringstream output;
    skai::Logger logger(output);
    {
        skai::Application app({}, logger, modules(events, joins));
        ASSERT_TRUE(app.initialize());
        ASSERT_TRUE(app.start());
    }
    EXPECT_EQ(joins, 4);
}
