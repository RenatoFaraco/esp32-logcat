#pragma once

#include "esp_system.h"

namespace logcat {

// The reset reason as the wire names it (wire-format §2.4: `info reset`, and the boot line):
// POWERON, USB, EXT, SW, PANIC, INT_WDT, TASK_WDT, WDT, BROWNOUT, DEEPSLEEP, SDIO, JTAG.
// nullptr for a reason without a name here (ESP_RST_UNKNOWN or a newer one).
const char* reset_name(esp_reset_reason_t reason);

}  // namespace logcat
