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
    std::vector<AlertEvent> find_by_time_range(std::int64_t from_ms,
                                                std::int64_t to_ms,
                                                std::size_t limit,
                                                std::string& error) const;
    std::vector<AlertEvent> find_by_class(const std::string& class_name,
                                           std::size_t limit,
                                           std::string& error) const;
    std::vector<AlertEvent> find_oldest_excess(std::size_t keep,
                                                std::string& error) const;
    bool remove(const std::string& id, std::string& error);
private:
    Database& database_;
};
} // namespace skai
