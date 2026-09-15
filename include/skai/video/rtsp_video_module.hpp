#pragma once

#include "skai/application.hpp"
#include "skai/video/rtsp_source.hpp"

#include <memory>

namespace skai {

// Application lifecycle adapter for the single configured RTSP input.
class RtspVideoModule final : public LifecycleModule {
public:
    RtspVideoModule(BoundedQueue<Frame>& frames, Logger& logger)
        : frames_(frames), logger_(logger) {}

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

private:
    BoundedQueue<Frame>& frames_;
    Logger& logger_;
    VideoConfig config_;
    std::unique_ptr<RtspSource> source_;
};

} // namespace skai
