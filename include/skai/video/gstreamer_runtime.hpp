#pragma once

#include "skai/logging.hpp"

#include <gst/gst.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace skai {
namespace gst {

bool initialize_once(std::string& error);

struct ObjectDeleter {
    template <typename T>
    void operator()(T* object) const noexcept {
        if (object) gst_object_unref(GST_OBJECT(object));
    }
};

using ElementPtr = std::unique_ptr<GstElement, ObjectDeleter>;
using BusPtr = std::unique_ptr<GstBus, ObjectDeleter>;

enum class BusEventType { None, StateChanged, Eos, Error };

struct BusEvent {
    BusEventType type = BusEventType::None;
    std::string detail;
    GstState old_state = GST_STATE_VOID_PENDING;
    GstState new_state = GST_STATE_VOID_PENDING;
};

class Pipeline {
public:
    static std::unique_ptr<Pipeline> create_empty(Logger& logger, std::string& error);
    static std::unique_ptr<Pipeline> from_launch(const std::string& launch,
                                                  Logger& logger, std::string& error);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    bool start(std::chrono::milliseconds timeout,
               const std::function<bool()>& cancelled = {});
    BusEvent poll(std::chrono::milliseconds timeout);
    void stop() noexcept;
    const std::string& last_error() const { return last_error_; }
    GstElement* element() const { return element_.get(); } // borrowed
    GstBus* bus() const { return bus_.get(); } // borrowed

private:
    Pipeline(ElementPtr element, BusPtr bus, Logger& logger);

    ElementPtr element_;
    BusPtr bus_;
    Logger& logger_;
    std::string last_error_;
    bool stopped_ = false;
};

} // namespace gst
} // namespace skai
