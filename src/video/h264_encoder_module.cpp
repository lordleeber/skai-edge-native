#include "skai/video/h264_encoder_module.hpp"

namespace skai {

bool H264EncoderModule::initialize(const Config&) {
    if (encoder_) return false;
    encoded_access_units_.reset();
    if (status_) status_->clear_encoder();
    encoder_ = std::make_unique<H264Encoder>(annotated_frames_, encoded_access_units_,
                                             logger_, status_);
    last_error_.clear();
    return true;
}

bool H264EncoderModule::start() {
    if (!encoder_) return false;
    if (encoder_->start(config_, last_error_)) return true;
    logger_.log(LogLevel::Error, "encoder", last_error_);
    return false;
}

void H264EncoderModule::stop() noexcept {
    if (encoder_) encoder_->request_stop();
}

void H264EncoderModule::wait() noexcept {
    if (encoder_) encoder_->stop();
    encoder_.reset();
    if (status_) status_->clear_encoder();
}

} // namespace skai
