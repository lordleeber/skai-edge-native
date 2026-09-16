#pragma once

#include <boost/beast/http.hpp>

#include <cstdint>
#include <string>

namespace skai::web {

namespace http = boost::beast::http;

struct StatusSnapshot {
    std::string status = "running";
    std::uint64_t uptime_s = 0;
    double video_fps = 0.0;
    double detector_fps = 0.0;
    double last_inference_ms = 0.0;
};

using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

Response route_request(const Request& request, const StatusSnapshot& status);

} // namespace skai::web
