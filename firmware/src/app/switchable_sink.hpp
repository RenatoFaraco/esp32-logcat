#pragma once

#include <atomic>
#include <cstddef>

#include "logcat/sink.hpp"

namespace app {

// Lets the demo turn a sink on and off at run time (the UART mirror cannot keep up with
// 1000 lines/s: ~70 lines/s at 115200 baud). Off = the line is accepted and discarded, so the
// writer does not count it as "not sent": the mirror is off by choice, not failing.
class SwitchableSink final : public logcat::Sink {
public:
    explicit SwitchableSink(logcat::Sink& inner) : inner_(inner) {}

    void set_enabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    bool write(const char* line, size_t length) override {
        return !enabled_.load(std::memory_order_relaxed) || inner_.write(line, length);
    }
    const char* name() const override { return inner_.name(); }

private:
    logcat::Sink& inner_;
    std::atomic<bool> enabled_{true};
};

}  // namespace app
