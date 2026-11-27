#pragma once

#include <cstdint>

namespace logcat {

// Milliseconds since boot, wrapping after ~49.7 days like LogRecord::uptime_ms.
class Clock {
public:
    virtual uint32_t uptime_ms() = 0;

protected:
    ~Clock() = default;
};

}  // namespace logcat
