#pragma once

#include <boost/beast/http.hpp>

#include "skai/api_state.hpp"
#include "skai/metrics.hpp"
#include "skai/storage/alert_repository.hpp"
#include "skai/status.hpp"
#include "skai/video/recording_control.hpp"
#include "skai/webrtc/webrtc_manager.hpp"

namespace skai::web {

namespace http = boost::beast::http;

using StatusSnapshot = RuntimeStatusSnapshot;

using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

std::string webrtc_diagnostics_json(const WebRtcDiagnostics& diagnostics);

Response route_request(const Request& request, const StatusSnapshot& status,
                       ApiState& api, AlertRepository* alerts = nullptr,
                       RecordingController* recording = nullptr,
                       WebRtcManager* webrtc = nullptr,
                       const MetricsSnapshot* metrics = nullptr);

} // namespace skai::web
