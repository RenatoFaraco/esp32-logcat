#pragma once

#include <cstdint>

#include "esp_timer.h"
#include "logcat/clock.hpp"

namespace logcat {

class EspClock final : public Clock {
public:
    uint32_t uptime_ms() override { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
};

}  // namespace logcat
