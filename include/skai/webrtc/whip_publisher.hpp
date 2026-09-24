#pragma once

#include "skai/application.hpp"
#include "skai/core/bounded_queue.hpp"
#include "skai/video/encoded_access_unit.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace skai {

// Publishes the existing Annex-B H.264 stream to one WHIP endpoint.
class WhipPublisher final : public LifecycleModule {
public:
    explicit WhipPublisher(Logger& logger) : logger_(logger) {}
    ~WhipPublisher() override { stop(); wait(); }

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;
    std::string last_error() const override { return last_error_; }
    void publish_access_unit(const EncodedAccessUnit& unit) noexcept;

private:
    void run() noexcept;
    void publish_once();

    Logger& logger_;
    WhipConfig config_;
    std::string token_;
    std::string last_error_;
    BoundedQueue<EncodedAccessUnit> media_queue_{8};
    std::atomic<bool> stopping_{false};
    std::mutex wait_mutex_;
    std::condition_variable wait_changed_;
    std::thread worker_;
};

} // namespace skai
