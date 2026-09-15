#pragma once

#include <gst/rtsp-server/rtsp-server.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace skai {
namespace test {

// Test-only loopback RTSP server; never link this class into skai-edge.
class RtspTestServer {
public:
    enum class Codec { H264, H265 };

    explicit RtspTestServer(Codec codec = Codec::H264) : codec_(codec) {}
    ~RtspTestServer();

    RtspTestServer(const RtspTestServer&) = delete;
    RtspTestServer& operator=(const RtspTestServer&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool set_stalled(bool stalled);
    // The first bound endpoint remains available across stop/start cycles.
    int port() const { return port_; }
    std::string url() const;

private:
    static void on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia*, gpointer data);

    Codec codec_;
    GstRTSPServer* server_ = nullptr;
    GMainContext* context_ = nullptr;
    GSource* source_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex gate_mutex_;
    GstElement* gate_ = nullptr;
    bool stalled_ = false;
    int port_ = 0;
};

} // namespace test
} // namespace skai
