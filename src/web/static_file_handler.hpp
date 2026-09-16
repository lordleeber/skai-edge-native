#pragma once

#include "skai/web/router.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace skai::web {

class StaticFileHandler {
public:
    explicit StaticFileHandler(const std::filesystem::path& root);

    bool valid() const noexcept;
    const std::string& error() const noexcept;
    Response handle(const Request& request) const;

private:
    struct Asset {
        std::string body;
        std::string content_type;
    };

    std::filesystem::path root_;
    std::unordered_map<std::string, Asset> assets_;
    std::string error_;
};

} // namespace skai::web
