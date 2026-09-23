#include "skai/webrtc/ice_runtime_module.hpp"

#include <skai_ice/host_interfaces.hpp>
#include <skai_ice/ice_log.hpp>

#include <net/if.h>

#include <sstream>
#include <vector>

namespace skai {

bool IceRuntimeModule::initialize(const Config& config) {
    last_error_.clear();
    bool local_interface_found = false;
    for (const auto& name : config.webrtc.host_interfaces) {
        local_interface_found |= if_nametoindex(name.c_str()) != 0;
    }
    if (config.webrtc.enabled && !local_interface_found) {
        std::ostringstream error;
        error << "webrtc.host_interfaces has no interface present on this host: [";
        for (std::size_t index = 0; index < config.webrtc.host_interfaces.size(); ++index) {
            if (index != 0) error << ',';
            error << config.webrtc.host_interfaces[index];
        }
        error << ']';
        last_error_ = error.str();
        return false;
    }

    std::vector<const char*> interface_names;
    interface_names.reserve(config.webrtc.host_interfaces.size());
    for (const auto& name : config.webrtc.host_interfaces) {
        interface_names.push_back(name.c_str());
    }
    skai_ice_set_host_interfaces(interface_names.data(),
                                 static_cast<int>(interface_names.size()));
    skai_ice_set_log_verbosity(config.webrtc.ice_log_verbosity);
    skai_ice::SetIceLogSink([this](int detail, const std::string& message) {
        const bool failure = message.find("FAILED") != std::string::npos ||
                             message.find("failed") != std::string::npos ||
                             message.find("error") != std::string::npos;
        logger_.log(failure ? LogLevel::Error
                            : detail > 1 ? LogLevel::Debug : LogLevel::Info,
                    "ice", message);
    });

    std::ostringstream message;
    message << (config.webrtc.enabled ? "configured" : "disabled")
            << " mode=lan-only"
            << " host interfaces=[";
    for (std::size_t index = 0; index < config.webrtc.host_interfaces.size(); ++index) {
        if (index != 0) message << ',';
        message << config.webrtc.host_interfaces[index];
    }
    message << "] ice_log_verbosity=" << config.webrtc.ice_log_verbosity;
    logger_.log(LogLevel::Info, "webrtc", message.str());
    return true;
}

void IceRuntimeModule::stop() noexcept {
    skai_ice::SetIceLogSink({});
}

} // namespace skai
