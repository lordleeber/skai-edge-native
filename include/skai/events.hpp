#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace skai {

enum class EventType { Status, Detection, Gps, Alert, Recording, SystemError };

const char* event_type_name(EventType type) noexcept;
std::string make_event_json(EventType type, const std::string& data_json);
std::string make_system_error_data(const std::string& module,
                                   const std::string& message);

class EventChannel {
public:
    using Subscriber = std::function<void(const std::string&)>;

    std::uint64_t subscribe(Subscriber subscriber);
    void unsubscribe(std::uint64_t id);
    void publish(EventType type, const std::string& data_json) const;

private:
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, Subscriber> subscribers_;
};

class EventQueue {
public:
    explicit EventQueue(std::size_t capacity);
    void push(std::string event);
    std::optional<std::string> pop();
    std::size_t size() const noexcept { return events_.size(); }
    std::size_t dropped() const noexcept { return dropped_; }

private:
    std::size_t capacity_;
    std::size_t dropped_ = 0;
    std::deque<std::string> events_;
};

} // namespace skai
