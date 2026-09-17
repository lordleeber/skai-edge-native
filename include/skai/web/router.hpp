#pragma once

#include <boost/beast/http.hpp>

#include "skai/api_state.hpp"
#include "skai/storage/alert_repository.hpp"
#include "skai/status.hpp"

namespace skai::web {

namespace http = boost::beast::http;

using StatusSnapshot = RuntimeStatusSnapshot;

using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

Response route_request(const Request& request, const StatusSnapshot& status,
                       ApiState& api, AlertRepository* alerts = nullptr);

} // namespace skai::web
