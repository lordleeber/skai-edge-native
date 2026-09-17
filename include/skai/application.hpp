#pragma once

#include "skai/config.hpp"
#include "skai/logging.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace skai {

// A module initializes resources before start launches workers. If initialize
// fails or throws, the module cleans up its own partial state. Once initialize
// succeeds, stop requests shutdown without blocking and wait joins workers and
// releases resources. Both must be safe even if start later fails.
class LifecycleModule {
public:
    virtual ~LifecycleModule() = default;
    virtual bool initialize(const Config& config) = 0;
    virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    virtual void wait() noexcept = 0;
    virtual std::string last_error() const { return {}; }
};

class Application {
public:
    struct Modules {
        std::unique_ptr<LifecycleModule> storage;
        std::unique_ptr<LifecycleModule> web;
        std::unique_ptr<LifecycleModule> detector;
        std::unique_ptr<LifecycleModule> video;
        std::unique_ptr<LifecycleModule> gps;
    };

    Application(std::string config_path, Logger& logger);
    Application(std::string config_path, Logger& logger, Modules modules);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool initialize();
    bool start();
    void stop() noexcept;
    void wait() noexcept;
    std::string last_error() const;
    std::string last_error_module() const;

private:
    struct ModuleRef {
        const char* name;
        LifecycleModule* module;
    };
    enum class State { New, Initialized, Running, Stopping, Stopped, Joining };

    std::string config_path_;
    Logger& logger_;
    Modules modules_;
    Config config_;
    std::vector<ModuleRef> active_;
    mutable std::mutex mutex_;
    std::condition_variable state_changed_;
    State state_ = State::New;
    std::string last_error_;
    std::string last_error_module_ = "core";
};

} // namespace skai
