#include "skai/storage/database.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <utility>

namespace skai {
namespace {

constexpr int current_schema_version = 1;

constexpr const char* migration_v1 = R"sql(
CREATE TABLE alerts (
    id TEXT PRIMARY KEY,
    timestamp_ms INTEGER NOT NULL,
    latitude REAL,
    longitude REAL,
    altitude_m REAL,
    gps_valid INTEGER NOT NULL DEFAULT 0,
    gps_source TEXT NOT NULL DEFAULT 'none',
    snapshot_path TEXT NOT NULL,
    frame_sequence INTEGER NOT NULL,
    model_version TEXT
);
CREATE TABLE detections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    alert_id TEXT NOT NULL,
    class_id INTEGER NOT NULL,
    class_name TEXT NOT NULL CHECK(length(class_name) > 0),
    confidence REAL NOT NULL,
    x1 REAL NOT NULL,
    y1 REAL NOT NULL,
    x2 REAL NOT NULL,
    y2 REAL NOT NULL,
    FOREIGN KEY(alert_id) REFERENCES alerts(id) ON DELETE CASCADE
);
CREATE INDEX idx_alerts_timestamp ON alerts(timestamp_ms DESC);
CREATE INDEX idx_detections_alert_id ON detections(alert_id);
CREATE INDEX idx_detections_class_name ON detections(class_name);
)sql";

bool execute(sqlite3* connection, const char* sql, std::string& error) {
    char* message = nullptr;
    const int result = sqlite3_exec(connection, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) return true;
    error = message ? message : sqlite3_errmsg(connection);
    sqlite3_free(message);
    return false;
}

} // namespace

Database::Database(std::string path)
    : path_(std::move(path)), path_from_config_(path_.empty()) {}

Database::~Database() { close(); }

bool Database::open(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_locked(error);
}

bool Database::open_locked(std::string& error) {
    error.clear();
    if (connection_) return true;
    if (path_.empty()) {
        error = "database path must not be empty";
        return false;
    }
    if (path_ != ":memory:") {
        std::error_code filesystem_error;
        const auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create database directory: " + filesystem_error.message();
            return false;
        }
    }
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path_.c_str(), &connection_, flags, nullptr) != SQLITE_OK) {
        error = connection_ ? sqlite3_errmsg(connection_) : "could not open database";
        if (connection_) sqlite3_close_v2(connection_);
        connection_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(connection_, 5000);
    if (!execute(connection_, "PRAGMA foreign_keys = ON;", error)) {
        sqlite3_close_v2(connection_);
        connection_ = nullptr;
        return false;
    }
    if (path_ != ":memory:") {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(connection_, "PRAGMA journal_mode = WAL;", -1,
                               &statement, nullptr) != SQLITE_OK) {
            error = sqlite3_errmsg(connection_);
        } else if (sqlite3_step(statement) != SQLITE_ROW) {
            error = sqlite3_errmsg(connection_);
        } else {
            const auto* value = sqlite3_column_text(statement, 0);
            const std::string selected = value
                ? reinterpret_cast<const char*>(value) : std::string{};
            if (sqlite3_stricmp(selected.c_str(), "wal") != 0) {
                error = "could not enable WAL journal mode; SQLite selected '" +
                        selected + "'";
            }
        }
        sqlite3_finalize(statement);
    }
    if (!error.empty() || !migrate_locked(error)) {
        sqlite3_close_v2(connection_);
        connection_ = nullptr;
        return false;
    }
    return true;
}

bool Database::migrate_locked(std::string& error) {
    if (!execute(connection_,
                 "CREATE TABLE IF NOT EXISTS schema_migrations ("
                 "version INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                 "applied_at_ms INTEGER NOT NULL);", error)) return false;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(connection_, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;",
                           -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(connection_);
        return false;
    }
    const int result = sqlite3_step(statement);
    const int version = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(connection_);
        return false;
    }
    if (version > current_schema_version) {
        error = "database schema is newer than this application";
        return false;
    }
    if (version == current_schema_version) return true;
    if (!execute(connection_, "BEGIN IMMEDIATE;", error)) return false;
    if (!execute(connection_, migration_v1, error)) {
        std::string ignored;
        execute(connection_, "ROLLBACK;", ignored);
        return false;
    }
    const auto applied_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    statement = nullptr;
    if (sqlite3_prepare_v2(connection_,
            "INSERT INTO schema_migrations(version, name, applied_at_ms) VALUES(1, 'v1_initial', ?);",
            -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(connection_);
    } else {
        sqlite3_bind_int64(statement, 1, applied_at);
        if (sqlite3_step(statement) != SQLITE_DONE) error = sqlite3_errmsg(connection_);
    }
    sqlite3_finalize(statement);
    if (!error.empty() || !execute(connection_, "COMMIT;", error)) {
        std::string ignored;
        execute(connection_, "ROLLBACK;", ignored);
        return false;
    }
    return true;
}

void Database::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_) sqlite3_close_v2(connection_);
    connection_ = nullptr;
}

bool Database::is_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_ != nullptr;
}

int Database::schema_version(std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (!connection_) {
        error = "database is not open";
        return -1;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(connection_, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;",
                           -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(connection_);
        return -1;
    }
    const int result = sqlite3_step(statement);
    const int version = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    if (result != SQLITE_ROW) error = sqlite3_errmsg(connection_);
    sqlite3_finalize(statement);
    return version;
}

bool Database::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
    if (connection_) {
        last_error_ = "database is already open";
        return false;
    }
    if (path_from_config_) path_ = config.storage.database_path;
    return open_locked(last_error_);
}

bool Database::start() { return is_open(); }
void Database::stop() noexcept {}
void Database::wait() noexcept { close(); }

std::string Database::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

} // namespace skai
