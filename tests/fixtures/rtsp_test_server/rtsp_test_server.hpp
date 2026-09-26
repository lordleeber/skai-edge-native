#pragma once

#include <gst/rtsp-server/rtsp-server.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace skai {
namespace test {

// Test-only loopback RTSP server; never link this class into skai-edge.
class RtspTestServer {
public:
    enum class Codec { H264, H264High, H265 };

    explicit RtspTestServer(Codec codec = Codec::H264, std::string username = {},
                            std::string password = {})
        : codec_(codec), username_(std::move(username)), password_(std::move(password)) {}
    ~RtspTestServer();

    RtspTestServer(const RtspTestServer&) = delete;
    RtspTestServer& operator=(const RtspTestServer&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool set_stalled(bool stalled);
    bool set_codec(Codec codec) {
        if (server_) return false;
        codec_ = codec;
        return true;
    }
    bool set_framerate(int fps) {
        if (server_ || fps <= 0) return false;
        framerate_ = fps;
        return true;
    }
    // Still publishes through RTSP; file input exists only inside the fixture.
    bool set_image(std::string path) {
        if (server_ || path.empty()) return false;
        image_path_ = std::move(path);
        return true;
    }
    // The first bound endpoint remains available across stop/start cycles.
    int port() const { return port_; }
    std::string url() const;

private:
    static void on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia*, gpointer data);

    Codec codec_;
    int framerate_ = 10;
    std::string image_path_;
    std::string username_;
    std::string password_;
    GstRTSPServer* server_ = nullptr;
    GMainContext* context_ = nullptr;
    GSource* source_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex gate_mutex_;
    GstElement* gate_ = nullptr;
    GstRTSPMedia* media_ = nullptr;
    bool stalled_ = false;
    int port_ = 0;
};

} // namespace test
} // namespace skai
