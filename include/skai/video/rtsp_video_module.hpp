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
    RtspVideoModule(BoundedQueue<Frame>& frames,
                    BoundedQueue<EncodedAccessUnit>& encoded_access_units,
                    Logger& logger, std::shared_ptr<RuntimeStatus> status = {},
                    RtspSource::AccessUnitSink access_unit_sink = {},
                    RtspSource::MediaStatusSink media_status_sink = {})
        : frames_(frames), encoded_access_units_(&encoded_access_units),
          logger_(logger), status_(std::move(status)),
          access_unit_sink_(std::move(access_unit_sink)),
          media_status_sink_(std::move(media_status_sink)) {}

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

private:
    BoundedQueue<Frame>& frames_;
    BoundedQueue<EncodedAccessUnit>* encoded_access_units_ = nullptr;
    Logger& logger_;
    std::shared_ptr<RuntimeStatus> status_;
    RtspSource::AccessUnitSink access_unit_sink_;
    RtspSource::MediaStatusSink media_status_sink_;
    VideoConfig config_;
    std::unique_ptr<RtspSource> source_;
};

} // namespace skai
