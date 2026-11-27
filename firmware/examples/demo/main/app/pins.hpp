#pragma once

#include "hal/gpio_types.h"

namespace app {

// Logic analyzer pins of the timing demo (#13, #14). Free on this board: not strapping
// (2, 8, 9), not USB (18, 19), not UART0 (20, 21), not the embedded flash (11-17)
// (docs/context/hardware.md).
inline constexpr gpio_num_t kPinTimer = GPIO_NUM_4;  // A: toggles in the timer ISR
inline constexpr gpio_num_t kPinTask = GPIO_NUM_5;   // B: toggles when the critical task runs
inline constexpr gpio_num_t kPinPhase = GPIO_NUM_6;  // C: N pulses at the start of phase N

}  // namespace app
