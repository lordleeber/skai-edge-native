#pragma once

#include "skai/application.hpp"

#include <mutex>
#include <string>

struct sqlite3;

namespace skai {

class AlertRepository;

class Database final : public LifecycleModule {
public:
    explicit Database(std::string path = {});
    ~Database() override;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(std::string& error);
    void close() noexcept;
    bool is_open() const noexcept;
    int schema_version(std::string& error) const;

    bool initialize(const Config& config) override;
    bool start() override;
    void stop() noexcept override;
    void wait() noexcept override;

private:
    friend class AlertRepository;

    bool open_locked(std::string& error);
    bool migrate_locked(std::string& error);

    std::string path_;
    mutable std::mutex mutex_;
    sqlite3* connection_ = nullptr;
};

} // namespace skai
