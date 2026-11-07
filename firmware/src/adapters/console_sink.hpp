#pragma once

#include <cstddef>

#include "esp_err.h"
#include "logcat/sink.hpp"

namespace logcat {

// The ESP-IDF console (stdout = UART0, the CH340K bridge on this board), so a monitor on the
// UART port keeps seeing the logs next to bootloader and printf output.
//
// begin() installs the UART driver and routes stdout through it. Without the driver, the VFS
// writes by polling the FIFO in a loop that never yields, ~15 ms of busy CPU per line at
// 115200 baud: under sustained logging the writer task would never block and the IDLE task
// would starve (task watchdog). With the driver, a full TX buffer makes the writer *sleep*.
//
// Known limit: the UART still drains only ~70 lines/s at 115200 baud, so with this mirror the
// writer task handles at most that many lines per second. Faster bursts fill the queue and are
// dropped and reported (ADR-004): visible, never blocking a caller. A fast application (#13)
// can leave this sink out. The console turns '\n' into "\r\n" (the host normalizes it); files
// in flash (M4) get their own sink, without '\r'.
class ConsoleSink final : public Sink {
public:
    static constexpr int kTxBufferSize = 2048;  // ~12 lines of burst before the writer waits
    static constexpr int kRxBufferSize = 256;   // must exceed the 128-byte hardware FIFO

    // Call at boot, before the writer task: allocates the driver's buffers once.
    esp_err_t begin();

    bool write(const char* line, size_t length) override;
    const char* name() const override { return "console"; }
};

}  // namespace logcat
