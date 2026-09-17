#include "skai/storage/alert_repository.hpp"

#include "skai/storage/database.hpp"

#include <sqlite3.h>

#include <limits>
#include <mutex>

namespace skai {
namespace {

class Statement {
public:
    Statement(sqlite3* connection, const char* sql, std::string& error) {
        if (sqlite3_prepare_v2(connection, sql, -1, &value_, nullptr) != SQLITE_OK) {
            error = sqlite3_errmsg(connection);
        }
    }
    ~Statement() { sqlite3_finalize(value_); }
    sqlite3_stmt* get() const { return value_; }

private:
    sqlite3_stmt* value_ = nullptr;
};

bool execute(sqlite3* connection, const char* sql, std::string& error) {
    char* message = nullptr;
    const int result = sqlite3_exec(connection, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) return true;
    error = message ? message : sqlite3_errmsg(connection);
    sqlite3_free(message);
    return false;
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* text = sqlite3_column_text(statement, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

AlertEvent read_alert(sqlite3_stmt* statement) {
    AlertEvent alert;
    alert.id = column_text(statement, 0);
    alert.timestamp_ms = sqlite3_column_int64(statement, 1);
    if (sqlite3_column_int(statement, 5) != 0) {
        alert.gps = GpsFix{true, column_text(statement, 6),
                           sqlite3_column_double(statement, 2),
                           sqlite3_column_double(statement, 3),
                           sqlite3_column_double(statement, 4)};
    }
    alert.snapshot_path = column_text(statement, 7);
    alert.frame_sequence = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 8));
    if (sqlite3_column_type(statement, 9) != SQLITE_NULL) {
        alert.model_version = column_text(statement, 9);
    }
    return alert;
}

bool read_detections(sqlite3* connection, AlertEvent& alert, std::string& error) {
    Statement query(connection,
        "SELECT class_id, class_name, confidence, x1, y1, x2, y2 "
        "FROM detections WHERE alert_id = ? ORDER BY id;", error);
    if (!query.get()) return false;
    sqlite3_bind_text(query.get(), 1, alert.id.c_str(), -1, SQLITE_TRANSIENT);
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
        alert.detections.push_back({sqlite3_column_int(query.get(), 0),
                                    column_text(query.get(), 1),
                                    sqlite3_column_double(query.get(), 2),
                                    sqlite3_column_double(query.get(), 3),
                                    sqlite3_column_double(query.get(), 4),
                                    sqlite3_column_double(query.get(), 5),
                                    sqlite3_column_double(query.get(), 6)});
    }
    if (result == SQLITE_DONE) return true;
    error = sqlite3_errmsg(connection);
    return false;
}

int bounded_limit(std::size_t limit) {
    return limit > static_cast<std::size_t>(std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max() : static_cast<int>(limit);
}

template <typename Bind>
std::vector<AlertEvent> query_alerts(sqlite3* connection, const char* sql,
                                     Bind bind, std::string& error) {
    std::vector<AlertEvent> alerts;
    Statement query(connection, sql, error);
    if (!query.get()) return alerts;
    bind(query.get());
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
        alerts.push_back(read_alert(query.get()));
    }
    if (result != SQLITE_DONE) {
        error = sqlite3_errmsg(connection);
        return {};
    }
    for (auto& alert : alerts) {
        if (!read_detections(connection, alert, error)) return {};
    }
    return alerts;
}

} // namespace

AlertRepository::AlertRepository(Database& database) : database_(database) {}

bool AlertRepository::insert(const AlertEvent& alert, std::string& error) {
    std::lock_guard<std::mutex> lock(database_.mutex_);
    error.clear();
    auto* connection = database_.connection_;
    if (!connection) {
        error = "database is not open";
        return false;
    }
    if (alert.id.empty() || alert.snapshot_path.empty() ||
        alert.frame_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        error = "alert id, snapshot path, and signed frame sequence are required";
        return false;
    }
    if (!execute(connection, "BEGIN IMMEDIATE;", error)) return false;
    bool committed = false;
    const auto rollback = [&] {
        if (!committed) {
            std::string ignored;
            execute(connection, "ROLLBACK;", ignored);
        }
    };

    Statement insert_alert(connection,
        "INSERT INTO alerts(id, timestamp_ms, latitude, longitude, altitude_m, "
        "gps_valid, gps_source, snapshot_path, frame_sequence, model_version) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?);", error);
    if (!insert_alert.get()) {
        rollback();
        return false;
    }
    sqlite3_bind_text(insert_alert.get(), 1, alert.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_alert.get(), 2, alert.timestamp_ms);
    const bool gps_valid = alert.gps && alert.gps->valid;
    if (gps_valid) {
        sqlite3_bind_double(insert_alert.get(), 3, alert.gps->latitude);
        sqlite3_bind_double(insert_alert.get(), 4, alert.gps->longitude);
        sqlite3_bind_double(insert_alert.get(), 5, alert.gps->altitude_m);
    } else {
        sqlite3_bind_null(insert_alert.get(), 3);
        sqlite3_bind_null(insert_alert.get(), 4);
        sqlite3_bind_null(insert_alert.get(), 5);
    }
    sqlite3_bind_int(insert_alert.get(), 6, gps_valid ? 1 : 0);
    const auto gps_source = gps_valid ? alert.gps->source : std::string("none");
    sqlite3_bind_text(insert_alert.get(), 7, gps_source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_alert.get(), 8, alert.snapshot_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_alert.get(), 9, static_cast<std::int64_t>(alert.frame_sequence));
    if (alert.model_version) {
        sqlite3_bind_text(insert_alert.get(), 10, alert.model_version->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(insert_alert.get(), 10);
    }
    if (sqlite3_step(insert_alert.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(connection);
        rollback();
        return false;
    }

    Statement insert_detection(connection,
        "INSERT INTO detections(alert_id, class_id, class_name, confidence, x1, y1, x2, y2) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?);", error);
    if (!insert_detection.get()) {
        rollback();
        return false;
    }
    for (const auto& detection : alert.detections) {
        sqlite3_reset(insert_detection.get());
        sqlite3_clear_bindings(insert_detection.get());
        sqlite3_bind_text(insert_detection.get(), 1, alert.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert_detection.get(), 2, detection.class_id);
        sqlite3_bind_text(insert_detection.get(), 3, detection.class_name.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_double(insert_detection.get(), 4, detection.confidence);
        sqlite3_bind_double(insert_detection.get(), 5, detection.x1);
        sqlite3_bind_double(insert_detection.get(), 6, detection.y1);
        sqlite3_bind_double(insert_detection.get(), 7, detection.x2);
        sqlite3_bind_double(insert_detection.get(), 8, detection.y2);
        if (sqlite3_step(insert_detection.get()) != SQLITE_DONE) {
            error = sqlite3_errmsg(connection);
            rollback();
            return false;
        }
    }
    if (!execute(connection, "COMMIT;", error)) {
        rollback();
        return false;
    }
    committed = true;
    return true;
}

std::optional<AlertEvent> AlertRepository::find_by_id(const std::string& id,
                                                        std::string& error) const {
    std::lock_guard<std::mutex> lock(database_.mutex_);
    error.clear();
    auto* connection = database_.connection_;
    if (!connection) {
        error = "database is not open";
        return std::nullopt;
    }
    Statement query(connection,
        "SELECT id, timestamp_ms, latitude, longitude, altitude_m, gps_valid, gps_source, "
        "snapshot_path, frame_sequence, model_version FROM alerts WHERE id = ?;", error);
    if (!query.get()) return std::nullopt;
    sqlite3_bind_text(query.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int result = sqlite3_step(query.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(connection);
        return std::nullopt;
    }
    auto alert = read_alert(query.get());
    if (!read_detections(connection, alert, error)) return std::nullopt;
    return alert;
}

std::vector<AlertEvent> AlertRepository::find_recent(std::size_t limit,
                                                       std::string& error) const {
    std::lock_guard<std::mutex> lock(database_.mutex_);
    error.clear();
    auto* connection = database_.connection_;
    if (!connection) {
        error = "database is not open";
        return {};
    }
    return query_alerts(connection,
        "SELECT id, timestamp_ms, latitude, longitude, altitude_m, gps_valid, gps_source, "
        "snapshot_path, frame_sequence, model_version FROM alerts "
        "ORDER BY timestamp_ms DESC, id DESC LIMIT ?;",
        [limit](sqlite3_stmt* statement) {
            sqlite3_bind_int(statement, 1, bounded_limit(limit));
        }, error);
}

std::vector<AlertEvent> AlertRepository::find_by_time_range(
    std::int64_t from_ms, std::int64_t to_ms, std::size_t limit,
    std::string& error) const {
    std::lock_guard<std::mutex> lock(database_.mutex_);
    error.clear();
    auto* connection = database_.connection_;
    if (!connection) { error = "database is not open"; return {}; }
    if (from_ms > to_ms) { error = "from must not exceed to"; return {}; }
    return query_alerts(connection,
        "SELECT id, timestamp_ms, latitude, longitude, altitude_m, gps_valid, gps_source, "
        "snapshot_path, frame_sequence, model_version FROM alerts "
        "WHERE timestamp_ms >= ? AND timestamp_ms <= ? "
        "ORDER BY timestamp_ms DESC, id DESC LIMIT ?;",
        [from_ms, to_ms, limit](sqlite3_stmt* statement) {
            sqlite3_bind_int64(statement, 1, from_ms);
            sqlite3_bind_int64(statement, 2, to_ms);
            sqlite3_bind_int(statement, 3, bounded_limit(limit));
        }, error);
}

std::vector<AlertEvent> AlertRepository::find_by_class(
    const std::string& class_name, std::size_t limit, std::string& error) const {
    std::lock_guard<std::mutex> lock(database_.mutex_);
    error.clear();
    auto* connection = database_.connection_;
    if (!connection) { error = "database is not open"; return {}; }
    return query_alerts(connection,
        "SELECT id, timestamp_ms, latitude, longitude, altitude_m, gps_valid, gps_source, "
        "snapshot_path, frame_sequence, model_version FROM alerts a "
        "WHERE EXISTS (SELECT 1 FROM detections d WHERE d.alert_id=a.id AND d.class_name=?) "
        "ORDER BY timestamp_ms DESC, id DESC LIMIT ?;",
        [&class_name, limit](sqlite3_stmt* statement) {
            sqlite3_bind_text(statement, 1, class_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 2, bounded_limit(limit));
        }, error);
}

std::vector<AlertEvent> AlertRepository::find_oldest_excess(
    std::size_t keep, std::string& error) const {
    std::lock_guard<std::mutex> lock(database_.mutex_);
    error.clear();
    auto* connection = database_.connection_;
    if (!connection) { error = "database is not open"; return {}; }
    return query_alerts(connection,
        "SELECT id, timestamp_ms, latitude, longitude, altitude_m, gps_valid, gps_source, "
        "snapshot_path, frame_sequence, model_version FROM alerts "
        "WHERE id NOT IN (SELECT id FROM alerts ORDER BY timestamp_ms DESC, id DESC LIMIT ?) "
        "ORDER BY timestamp_ms ASC, id ASC;",
        [keep](sqlite3_stmt* statement) {
            sqlite3_bind_int(statement, 1, bounded_limit(keep));
        }, error);
}

bool AlertRepository::remove(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(database_.mutex_);
    error.clear();
    auto* connection = database_.connection_;
    if (!connection) { error = "database is not open"; return false; }
    Statement statement(connection, "DELETE FROM alerts WHERE id = ?;", error);
    if (!statement.get()) return false;
    sqlite3_bind_text(statement.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(connection);
        return false;
    }
    return sqlite3_changes(connection) == 1;
}

} // namespace skai
