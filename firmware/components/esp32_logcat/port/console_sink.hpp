#pragma once

#include <cstddef>

#include "esp_err.h"
#include "logcat/sink.hpp"

namespace logcat {

// The ESP-IDF console (stdout = UART0, the CH340K bridge on this board), so a monitor on the
// UART port keeps seeing the logs next to bootloader and printf output.
//
// begin() installs the UART driver and routes stdout through it, so printf waits on the driver
// instead of polling the FIFO in a loop that never yields.
//
// write() never waits: the whole line goes into the driver's TX ring, or it is refused and the
// Writer counts it (ADR-004), exactly like the USB sink. The UART drains only ~70 lines/s at
// 115200 baud, so a faster application loses lines on this mirror, visibly, and nothing else.
// Lines go out with CRLF (the host normalizes it); files in flash (M4) get their own sink.
class ConsoleSink final : public Sink {
public:
    static constexpr int kTxBufferSize = 2048;  // ~12 lines of burst, then refused
    static constexpr int kRxBufferSize = 256;   // must exceed the 128-byte hardware FIFO

    // Call at boot, before the writer task: allocates the driver's buffers once.
    esp_err_t begin();

    bool write(const char* line, size_t length) override;
    const char* name() const override { return "console"; }
};

}  // namespace logcat
