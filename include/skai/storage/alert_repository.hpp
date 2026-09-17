#pragma once
#include "skai/alerts/alert_event.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
namespace skai {
class Database;
class AlertRepository {
public:
    explicit AlertRepository(Database& database);
    bool insert(const AlertEvent& alert, std::string& error);
    std::optional<AlertEvent> find_by_id(const std::string& id,
                                          std::string& error) const;
    std::vector<AlertEvent> find_recent(std::size_t limit,
                                         std::string& error) const;
private:
    Database& database_;
};
} // namespace skai
