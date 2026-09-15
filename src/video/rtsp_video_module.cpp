#include "skai/video/rtsp_video_module.hpp"

#include <string>
#include <utility>

namespace skai {

bool RtspVideoModule::initialize(const Config& config) {
    if (source_) return false;
    auto source = std::make_unique<RtspSource>(frames_, logger_);
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
    const auto info = source_->diagnostics();
    const std::string fps = info.fps_num > 0
                                ? std::to_string(info.fps_num) + "/" +
                                      std::to_string(info.fps_den) + " fps"
                                : "unknown fps";
    logger_.log(LogLevel::Info, "video", "RTSP source connected: " + info.codec + " " +
                                         std::to_string(info.width) + "x" +
                                         std::to_string(info.height) + " " + fps);
    return true;
}

void RtspVideoModule::stop() noexcept {
    if (source_) source_->request_stop();
}

void RtspVideoModule::wait() noexcept {
    if (source_) source_->stop();
    frames_.shutdown();
    source_.reset();
}

} // namespace skai
