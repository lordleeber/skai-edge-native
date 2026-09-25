#include "skai/application.hpp"

#include <exception>
#include <utility>

namespace skai {
namespace {

template <typename Action>
bool try_action(Action action) {
    try {
        return action();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

Application::Application(std::string config_path, Logger& logger)
    : Application(std::move(config_path), logger, {}) {}

Application::Application(std::string config_path, Logger& logger, Modules modules)
    : config_path_(std::move(config_path)), logger_(logger), modules_(std::move(modules)) {}

Application::~Application() {
    stop();
    wait();
}

bool Application::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::New) {
        last_error_ = "application must be stopped and joined before initialization";
        last_error_module_ = "core";
        return false;
    }
    last_error_.clear();
    last_error_module_ = "core";
    const auto loaded = config_path_.empty()
                            ? ConfigResult{true, Config{}, {}}
                            : load_config(config_path_);
    if (!loaded.ok) {
        last_error_ = loaded.error;
        last_error_module_ = "config";
        return false;
    }
    config_ = loaded.config;
    logger_.set_minimum_level(config_.logging.level);
    logger_.log(LogLevel::Info, "config", config_path_.empty()
                    ? "using built-in defaults" : "configuration validated");

    const ModuleRef ordered[] = {
        {"storage", modules_.storage.get()}, {"webrtc", modules_.webrtc.get()},
        {"whip", modules_.whip.get()},
        {"web", modules_.web.get()},
        {"encoder", modules_.encoder.get()}, {"detector", modules_.detector.get()},
        {"recording", modules_.recording.get()},
        {"video", modules_.video.get()}, {"gps", modules_.gps.get()}};
    for (const auto& item : ordered) {
        if (!item.module) continue;
        if (try_action([&] { return item.module->initialize(config_); })) {
            active_.push_back(item);
            continue;
        }
        const auto detail = item.module->last_error();
        last_error_ = detail.empty() ? std::string(item.name) + ".initialize failed"
                                     : detail;
        last_error_module_ = item.name;
        for (auto it = active_.rbegin(); it != active_.rend(); ++it) it->module->stop();
        for (auto it = active_.rbegin(); it != active_.rend(); ++it) it->module->wait();
        active_.clear();
        return false;
    }
    state_ = State::Initialized;
    return true;
}

bool Application::start() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::Initialized) {
        last_error_ = "application must be initialized before start";
        last_error_module_ = "core";
        return false;
    }
    last_error_.clear();
    last_error_module_ = "core";
    for (const auto& item : active_) {
        if (try_action([&] { return item.module->start(); })) continue;
        last_error_ = std::string(item.name) + ".start failed";
        last_error_module_ = item.name;
        lock.unlock();
        stop();
        wait();
        return false;
    }
    state_ = State::Running;
    return true;
}

void Application::stop() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::Initialized && state_ != State::Running) return;
    state_ = State::Stopping;
    lock.unlock();
    for (auto it = active_.rbegin(); it != active_.rend(); ++it) it->module->stop();
    lock.lock();
    state_ = State::Stopped;
    lock.unlock();
    state_changed_.notify_all();
}

void Application::wait() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    state_changed_.wait(lock, [this] {
        return state_ == State::New || state_ == State::Stopped || state_ == State::Joining;
    });
    if (state_ == State::Joining) {
        state_changed_.wait(lock, [this] { return state_ != State::Joining; });
        return;
    }
    if (state_ == State::New) return;
    state_ = State::Joining;
    lock.unlock();
    for (auto it = active_.rbegin(); it != active_.rend(); ++it) it->module->wait();
    lock.lock();
    active_.clear();
    state_ = State::New;
    lock.unlock();
    state_changed_.notify_all();
}

std::string Application::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::string Application::last_error_module() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_module_;
}

Config Application::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

} // namespace skai
