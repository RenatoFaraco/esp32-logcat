#pragma once

#include <cstddef>

#include "esp_err.h"
#include "logcat/sink.hpp"

namespace logcat {

// The native USB port (USB-Serial-JTAG) through ESP-IDF's driver.
//
// Requires CONFIG_ESP_CONSOLE_SECONDARY_NONE: the secondary console would write the same
// hardware FIFO directly, mixing its bytes with ours, and it is what blocked ~50 ms per line
// when no host was reading (#12).
//
// write() copies the whole line into the driver's TX ring buffer or nothing (byte buffer,
// zero timeout): never half a line, never a wait. With no host reading, the ring fills and
// lines are refused whole; the Writer counts and reports them. Lines go out with '\n' only
// (no VFS newline translation), as wire-format §1 specifies.
class UsbJtagSink final : public Sink {
public:
    static constexpr size_t kTxBufferSize = 1024;  // ~6 lines of burst while the host is slow
    static constexpr size_t kRxBufferSize = 256;   // for the console commands (M5)

    // Installs the driver (allocates its ring buffers: at boot, never on the log path).
    esp_err_t begin();

    bool write(const char* line, size_t length) override;
    const char* name() const override { return "usb"; }
};

}  // namespace logcat
