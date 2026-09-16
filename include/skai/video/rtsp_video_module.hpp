#pragma once

#include "skai/application.hpp"
#include "skai/video/rtsp_source.hpp"

#include <memory>
#include <utility>

namespace skai {

// Application lifecycle adapter for the single configured RTSP input.
class RtspVideoModule final : public LifecycleModule {
public:
    RtspVideoModule(BoundedQueue<Frame>& frames, Logger& logger,
                    std::shared_ptr<RuntimeStatus> status = {})
        : frames_(frames), logger_(logger), status_(std::move(status)) {}

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

private:
    BoundedQueue<Frame>& frames_;
    Logger& logger_;
    std::shared_ptr<RuntimeStatus> status_;
    VideoConfig config_;
    std::unique_ptr<RtspSource> source_;
};

} // namespace skai
