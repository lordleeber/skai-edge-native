#include "skai/video/rtsp_video_module.hpp"

#include <string>
#include <utility>

namespace skai {

bool RtspVideoModule::initialize(const Config& config) {
    if (source_) return false;
    auto source = std::make_unique<RtspSource>(frames_, logger_, DecodeMode::Auto,
                                               true, status_);
    config_ = config.video;
    frames_.reset();
    source_ = std::move(source);
    return true;
}

bool RtspVideoModule::start() {
    if (!source_) return false;
    std::string error;
    if (!source_->start(config_, error)) {
        logger_.log(LogLevel::Error, "video", error);
        return false;
    }
    logger_.log(LogLevel::Info, "video", "RTSP source started; connecting");
    return true;
}

void RtspVideoModule::stop() noexcept {
    if (source_) source_->request_stop();
}

void RtspVideoModule::wait() noexcept {
    if (source_) source_->stop();
    if (status_) status_->clear_video();
    frames_.shutdown();
    source_.reset();
}

} // namespace skai
