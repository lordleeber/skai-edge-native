#pragma once

#include "skai/web/router.hpp"

#include <filesystem>
#include <string>

namespace skai::web {

class StaticFileHandler {
public:
    explicit StaticFileHandler(const std::filesystem::path& root);

    bool valid() const noexcept;
    const std::string& error() const noexcept;
    Response handle(const Request& request) const;

private:
    std::filesystem::path root_;
    std::string error_;
};

} // namespace skai::web
