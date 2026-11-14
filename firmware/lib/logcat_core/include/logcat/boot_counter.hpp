#pragma once

#include <cstdint>

#include "logcat/line_formatter.hpp"

namespace logcat {

// The boot counter (PRD F7, #25): one number per boot, stamped on every line (wire-format §1),
// so the logs kept in flash are told apart across resets.

// Boot 0 is never a real boot: it means the counter could not be read.
inline constexpr uint32_t kUnknownBoot = 0;
// The counter runs 1..9999 and then starts at 1 again: a line prints boot % 10000, so it never
// shows a real boot as 0000, which stays "unknown".
inline constexpr uint32_t kMaxBoot = kBootModulo - 1;

enum class BootLoad {
    Found,    // a value was stored
    Missing,  // nothing stored yet: the first boot
    Failed,   // the store could not be read
};

// Where the counter lives (NVS on the board). Called once, at boot, before the log hook.
class BootStore {
public:
    virtual BootLoad load(uint32_t& value) = 0;
    virtual bool save(uint32_t value) = 0;

protected:
    ~BootStore() = default;
};

struct BootInfo {
    uint32_t boot = kUnknownBoot;
    bool saved = false;  // false: the next boot may get the same number
    bool first = false;  // nothing was stored before
};

// Reads the previous count, adds one (after kMaxBoot, or from a value out of range, back to 1)
// and saves it. A failed read gives kUnknownBoot and saves nothing.
BootInfo next_boot(BootStore& store);

}  // namespace logcat
