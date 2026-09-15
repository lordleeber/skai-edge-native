#include "rtsp_test_server.hpp"

#include <gst/gst.h>

#include <exception>
#include <string>

namespace skai {
namespace test {
namespace {

bool plugin_available(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

GstRTSPFilterResult close_client(GstRTSPServer*, GstRTSPClient* client, gpointer) {
    gst_rtsp_client_close(client);
    return GST_RTSP_FILTER_REMOVE;
}

const char* launch_for(RtspTestServer::Codec codec) {
    if (codec == RtspTestServer::Codec::H265) {
        return "( videotestsrc is-live=true pattern=smpte ! "
               "video/x-raw,width=160,height=120,framerate=10/1 ! videoconvert ! "
               "identity name=stall_gate ! x265enc speed-preset=ultrafast "
               "tune=zerolatency bitrate=100 ! rtph265pay name=pay0 pt=96 "
               "config-interval=1 )";
    }
    return "( videotestsrc is-live=true pattern=smpte ! "
           "video/x-raw,width=160,height=120,framerate=10/1 ! videoconvert ! "
           "identity name=stall_gate ! x264enc speed-preset=ultrafast "
           "tune=zerolatency bitrate=100 key-int-max=10 ! "
           "rtph264pay name=pay0 pt=96 config-interval=1 )";
}

} // namespace

RtspTestServer::~RtspTestServer() { stop(); }

bool RtspTestServer::start(std::string& error) {
    if (server_) {
        error = "RTSP test server is already running";
        return false;
    }
    if (!gst_is_initialized()) {
        error = "GStreamer must be initialized before the RTSP test server";
        return false;
    }
    const bool plugins_ready = codec_ == Codec::H264
                                   ? plugin_available("x264enc") && plugin_available("rtph264pay")
                                   : plugin_available("x265enc") && plugin_available("rtph265pay");
    if (!plugins_ready) {
        error = "RTSP test encoder or payloader plugin is unavailable";
        return false;
    }

    server_ = gst_rtsp_server_new();
    if (!server_) {
        error = "could not create RTSP test server";
        return false;
    }
    gst_rtsp_server_set_address(server_, "127.0.0.1");
    const std::string service = port_ > 0 ? std::to_string(port_) : "0";
    gst_rtsp_server_set_service(server_, service.c_str());
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    if (!mounts || !factory) {
        if (mounts) gst_object_unref(mounts);
        if (factory) gst_object_unref(factory);
        error = "could not create RTSP test media factory";
        stop();
        return false;
    }
    gst_rtsp_media_factory_set_launch(factory, launch_for(codec_));
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    if (!username_.empty()) {
        GstRTSPAuth* auth = gst_rtsp_auth_new();
        GstRTSPToken* token = gst_rtsp_token_new(GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE,
                                                G_TYPE_STRING, "viewer", nullptr);
        gchar* basic = gst_rtsp_auth_make_basic(username_.c_str(), password_.c_str());
        gst_rtsp_auth_add_basic(auth, basic, token);
        gst_rtsp_token_unref(token);
        g_free(basic);
        gst_rtsp_server_set_auth(server_, auth);
        gst_object_unref(auth);

        GstRTSPPermissions* permissions = gst_rtsp_permissions_new();
        gst_rtsp_permissions_add_role(permissions, "viewer",
                                      GST_RTSP_PERM_MEDIA_FACTORY_ACCESS, G_TYPE_BOOLEAN, TRUE,
                                      GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, G_TYPE_BOOLEAN, TRUE,
                                      nullptr);
        gst_rtsp_media_factory_set_permissions(factory, permissions);
        gst_rtsp_permissions_unref(permissions);
    }
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), this);
    gst_rtsp_mount_points_add_factory(mounts, "/test", factory); // transfers factory
    gst_object_unref(mounts);

    context_ = g_main_context_new();
    if (!context_) {
        error = "could not create RTSP test main context";
        stop();
        return false;
    }
    GError* gst_error = nullptr;
    source_ = gst_rtsp_server_create_source(server_, nullptr, &gst_error);
    if (!source_) {
        error = gst_error ? gst_error->message : "could not bind RTSP test server";
        if (gst_error) g_error_free(gst_error);
        stop();
        return false;
    }
    if (gst_error) g_error_free(gst_error);
    if (g_source_attach(source_, context_) == 0) {
        error = "could not attach RTSP test server source";
        stop();
        return false;
    }
    const int bound_port = gst_rtsp_server_get_bound_port(server_);
    if (bound_port <= 0 || (port_ > 0 && bound_port != port_)) {
        error = "RTSP test server did not bind the requested port";
        stop();
        return false;
    }
    port_ = bound_port;
    try {
        running_ = true;
        worker_ = std::thread([this] {
            g_main_context_push_thread_default(context_);
            while (running_) g_main_context_iteration(context_, TRUE);
            g_main_context_pop_thread_default(context_);
        });
    } catch (const std::exception& failure) {
        error = failure.what();
        stop();
        return false;
    }
    error.clear();
    return true;
}

void RtspTestServer::stop() noexcept {
    running_ = false;
    if (source_) g_source_destroy(source_);
    if (context_) g_main_context_wakeup(context_);
    if (worker_.joinable()) worker_.join();
    if (server_) gst_rtsp_server_client_filter(server_, close_client, nullptr);
    GstRTSPMedia* media = nullptr;
    {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        media = media_;
        media_ = nullptr;
    }
    if (media) {
        gst_rtsp_media_unprepare(media);
        gst_object_unref(media);
    }
    {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        if (gate_) gst_object_unref(gate_);
        gate_ = nullptr;
        stalled_ = false;
    }
    if (source_) g_source_unref(source_);
    if (context_) g_main_context_unref(context_);
    if (server_) gst_object_unref(server_);
    source_ = nullptr;
    context_ = nullptr;
    server_ = nullptr;
}

bool RtspTestServer::set_stalled(bool stalled) {
    if (!server_) return false;
    std::lock_guard<std::mutex> lock(gate_mutex_);
    stalled_ = stalled;
    if (gate_) g_object_set(gate_, "drop-probability", stalled ? 1.0 : 0.0, nullptr);
    return true;
}

std::string RtspTestServer::url() const {
    return port_ > 0 ? "rtsp://127.0.0.1:" + std::to_string(port_) + "/test" : std::string{};
}

void RtspTestServer::on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia* media,
                                         gpointer data) {
    auto* self = static_cast<RtspTestServer*>(data);
    GstElement* element = gst_rtsp_media_get_element(media); // full reference
    if (!element) return;
    GstElement* gate = GST_IS_BIN(element)
                           ? gst_bin_get_by_name_recurse_up(GST_BIN(element), "stall_gate")
                           : nullptr;
    gst_object_unref(element);
    if (!gate) return;
    std::lock_guard<std::mutex> lock(self->gate_mutex_);
    if (self->media_) gst_object_unref(self->media_);
    self->media_ = GST_RTSP_MEDIA(gst_object_ref(media));
    if (self->gate_) gst_object_unref(self->gate_);
    self->gate_ = gate; // full reference
    g_object_set(gate, "drop-probability", self->stalled_ ? 1.0 : 0.0, nullptr);
}

} // namespace test
} // namespace skai
