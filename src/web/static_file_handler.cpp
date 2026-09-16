#include "static_file_handler.hpp"

#include <boost/beast/http.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace skai::web {
namespace {

namespace http = boost::beast::http;

Response text_response(http::status status, const std::string& message) {
    Response response{status, 11};
    response.set(http::field::content_type, "text/plain; charset=utf-8");
    response.body() = message + "\n";
    response.prepare_payload();
    return response;
}

bool decode_path(boost::beast::string_view target, std::string& decoded) {
    const auto query = target.find('?');
    target = target.substr(0, query);
    decoded.clear();
    decoded.reserve(target.size());
    const auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < target.size(); ++index) {
        if (target[index] != '%') {
            decoded.push_back(target[index]);
            continue;
        }
        if (index + 2 >= target.size()) return false;
        const auto high = hex(target[index + 1]);
        const auto low = hex(target[index + 2]);
        if (high < 0 || low < 0) return false;
        const auto value = static_cast<char>((high << 4) | low);
        if (value == '\0') return false;
        decoded.push_back(value);
        index += 2;
    }
    return !decoded.empty() && decoded.front() == '/';
}

bool is_within(const std::filesystem::path& root,
               const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *root_part != *candidate_part) {
            return false;
        }
    }
    return true;
}

const char* content_type(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".json") return "application/json";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    return "application/octet-stream";
}

} // namespace

StaticFileHandler::StaticFileHandler(const std::filesystem::path& root) {
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(root, error);
    if (error || !std::filesystem::is_directory(root_, error)) {
        error_ = "web.root must be an accessible directory";
    }
}

bool StaticFileHandler::valid() const noexcept {
    return error_.empty();
}

const std::string& StaticFileHandler::error() const noexcept {
    return error_;
}

Response StaticFileHandler::handle(const Request& request) const {
    if (request.method() != http::verb::get &&
        request.method() != http::verb::head) {
        auto response = text_response(http::status::method_not_allowed,
                                      "method not allowed");
        response.set(http::field::allow, "GET, HEAD");
        return response;
    }

    std::string decoded;
    if (!decode_path(request.target(), decoded)) {
        return text_response(http::status::bad_request, "invalid request target");
    }
    auto relative = decoded == "/" ? std::filesystem::path("index.html")
                                    : std::filesystem::path(decoded.substr(1));
    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(root_ / relative, error);
    if (error) return text_response(http::status::not_found, "not found");
    if (!is_within(root_, candidate)) {
        return text_response(http::status::forbidden, "forbidden");
    }
    if (!std::filesystem::is_regular_file(candidate, error) || error) {
        return text_response(http::status::not_found, "not found");
    }

    std::ifstream file(candidate, std::ios::binary);
    if (!file) return text_response(http::status::not_found, "not found");
    std::string body{std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>()};
    Response response{http::status::ok, request.version()};
    response.set(http::field::content_type, content_type(candidate));
    response.set(http::field::cache_control, "no-cache");
    if (request.method() == http::verb::head) {
        response.content_length(body.size());
    } else {
        response.body() = std::move(body);
        response.prepare_payload();
    }
    return response;
}

} // namespace skai::web
