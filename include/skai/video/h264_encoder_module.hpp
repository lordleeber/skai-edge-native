#pragma once

#include "skai/application.hpp"
#include "skai/core/bounded_queue.hpp"
#include "skai/status.hpp"
#include "skai/video/h264_encoder.hpp"

#include <memory>
#include <string>
#include <utility>

namespace skai {

// Lifecycle adapter between annotated frames and the shared encoded-AU queue.
// Recording and WebRTC attach as consumers of encoded_access_units rather than
// making the detector aware of either feature.
class H264EncoderModule final : public LifecycleModule {
public:
    H264EncoderModule(BoundedQueue<Frame>& annotated_frames,
                      BoundedQueue<EncodedAccessUnit>& encoded_access_units,
                      Logger& logger, std::shared_ptr<RuntimeStatus> status = {},
                      H264Encoder::AccessUnitSink access_unit_sink = {})
        : annotated_frames_(annotated_frames), encoded_access_units_(encoded_access_units),
          logger_(logger), status_(std::move(status)),
          access_unit_sink_(std::move(access_unit_sink)) {}

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;
    std::string last_error() const override { return last_error_; }

private:
    BoundedQueue<Frame>& annotated_frames_;
    BoundedQueue<EncodedAccessUnit>& encoded_access_units_;
    Logger& logger_;
    std::shared_ptr<RuntimeStatus> status_;
    H264Encoder::AccessUnitSink access_unit_sink_;
    H264EncoderConfig config_;
    std::unique_ptr<H264Encoder> encoder_;
    std::string last_error_;
};

} // namespace skai
