#include "skai/events.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <vector>

namespace skai {
namespace {

std::string json_string(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(character));
        } else if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else if (character == '\t') result += "\\t";
        else if (character >= 0x20) result.push_back(static_cast<char>(character));
    }
    result.push_back('"');
    return result;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
    return output.str();
}

} // namespace

const char* event_type_name(EventType type) noexcept {
    switch (type) {
    case EventType::Status: return "status";
    case EventType::Detection: return "detection";
    case EventType::Gps: return "gps";
    case EventType::Alert: return "alert";
    case EventType::Recording: return "recording";
    case EventType::SystemError: return "system_error";
    }
    return "system_error";
}

std::string make_event_json(EventType type, const std::string& data_json) {
    return std::string("{\"type\":\"") + event_type_name(type) +
           "\",\"timestamp\":\"" + timestamp() + "\",\"data\":" +
           data_json + "}\n";
}

std::string make_system_error_data(const std::string& module,
                                   const std::string& message) {
    return std::string("{\"module\":") + json_string(module) +
           ",\"message\":" + json_string(message) + '}';
}

std::uint64_t EventChannel::subscribe(Subscriber subscriber) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = next_id_++;
    subscribers_.emplace(id, std::move(subscriber));
    return id;
}

void EventChannel::unsubscribe(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(id);
}

void EventChannel::publish(EventType type, const std::string& data_json) const {
    const auto event = make_event_json(type, data_json);
    std::vector<Subscriber> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (type == EventType::SystemError) {
            if (recent_errors_.size() == 10) recent_errors_.pop_front();
            recent_errors_.push_back(event);
        }
        subscribers.reserve(subscribers_.size());
        for (const auto& item : subscribers_) subscribers.push_back(item.second);
    }
    for (const auto& subscriber : subscribers) subscriber(event);
}

std::vector<std::string> EventChannel::recent_errors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {recent_errors_.begin(), recent_errors_.end()};
}

EventQueue::EventQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("event queue capacity must be positive");
    }
}

void EventQueue::push(std::string event) {
    if (events_.size() == capacity_) {
        events_.pop_front();
        ++dropped_;
    }
    events_.push_back(std::move(event));
}

std::optional<std::string> EventQueue::pop() {
    if (events_.empty()) return std::nullopt;
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

EventInbox::EventInbox(std::size_t capacity) : events_(capacity) {}

bool EventInbox::push(std::string event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push(std::move(event));
    if (drain_scheduled_) return false;
    drain_scheduled_ = true;
    return true;
}

std::vector<std::string> EventInbox::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(events_.size());
    while (auto event = events_.pop()) result.push_back(std::move(*event));
    drain_scheduled_ = false;
    return result;
}

std::size_t EventInbox::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.dropped();
}

} // namespace skai
