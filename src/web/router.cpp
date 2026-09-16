#include "skai/web/router.hpp"

#include <iomanip>
#include <locale>
#include <sstream>

namespace skai::web {
namespace {

Response json_response(http::status result, unsigned version, std::string body) {
    Response response{result, version};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

std::string status_json(const StatusSnapshot& status) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(6)
           << "{\"status\":\"" << status.status << "\",\"uptime_s\":"
           << status.uptime_s << ",\"video\":{\"fps\":" << status.video_fps
           << "},\"detector\":{\"fps\":" << status.detector_fps
           << ",\"last_inference_ms\":" << status.last_inference_ms << "}}\n";
    return output.str();
}

} // namespace

Response route_request(const Request& request, const StatusSnapshot& status) {
    const auto target = request.target();
    if (target != "/health" && target != "/api/v1/status") {
        return json_response(http::status::not_found, request.version(),
                             "{\"error\":\"not found\"}\n");
    }
    if (request.method() != http::verb::get) {
        auto response = json_response(http::status::method_not_allowed,
                                      request.version(),
                                      "{\"error\":\"method not allowed\"}\n");
        response.set(http::field::allow, "GET");
        return response;
    }
    if (target == "/health") {
        return json_response(http::status::ok, request.version(),
                             "{\"status\":\"ok\"}\n");
    }
    return json_response(http::status::ok, request.version(), status_json(status));
}

} // namespace skai::web
