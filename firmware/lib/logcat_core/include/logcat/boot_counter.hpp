#pragma once

#include <cstdint>

namespace logcat {

// The boot counter (PRD F7, #25): one number per boot, stamped on every line (wire-format §1),
// so the logs kept in flash are told apart across resets.

// Boot 0 is never a real boot: it means the counter could not be read.
inline constexpr uint32_t kUnknownBoot = 0;

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

// Reads the previous count, adds one (wrapping to 1, never to 0) and saves it. A failed read
// gives kUnknownBoot and saves nothing.
BootInfo next_boot(BootStore& store);

}  // namespace logcat
