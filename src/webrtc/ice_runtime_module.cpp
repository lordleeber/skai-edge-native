#include "skai/webrtc/ice_runtime_module.hpp"

#include <skai_ice/host_interfaces.hpp>
#include <skai_ice/ice_log.hpp>

#include <sstream>
#include <vector>

namespace skai {

bool IceRuntimeModule::initialize(const Config& config) {
    std::vector<const char*> interface_names;
    interface_names.reserve(config.webrtc.host_interfaces.size());
    for (const auto& name : config.webrtc.host_interfaces) {
        interface_names.push_back(name.c_str());
    }
    skai_ice_set_host_interfaces(interface_names.data(),
                                 static_cast<int>(interface_names.size()));
    skai_ice_set_log_verbosity(config.webrtc.ice_log_verbosity);

    std::ostringstream message;
    message << (config.webrtc.enabled ? "configured" : "disabled")
            << " host interfaces=[";
    for (std::size_t index = 0; index < config.webrtc.host_interfaces.size(); ++index) {
        if (index != 0) message << ',';
        message << config.webrtc.host_interfaces[index];
    }
    message << "] ice_log_verbosity=" << config.webrtc.ice_log_verbosity;
    logger_.log(LogLevel::Info, "webrtc", message.str());
    return true;
}

} // namespace skai
