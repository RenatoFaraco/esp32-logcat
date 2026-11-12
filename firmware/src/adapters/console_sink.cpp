#include "adapters/console_sink.hpp"

#include <cstring>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "logcat/line_formatter.hpp"
#include "sdkconfig.h"

namespace logcat {

esp_err_t ConsoleSink::begin() {
    const auto port = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    if (!uart_is_driver_installed(port)) {
        const esp_err_t err =
            uart_driver_install(port, kRxBufferSize, kTxBufferSize, 0, nullptr, 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    uart_vfs_dev_use_driver(port);  // stdout/printf now wait on the driver, not on the FIFO
    return ESP_OK;
}

bool ConsoleSink::write(const char* line, size_t length) {
    const auto port = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    // Whole line or nothing, and only when it fits: uart_write_bytes() busy-waits on a full ring
    // without ever sleeping, and was seen stuck there for good, starving the IDLE task (#28).
    size_t free_size = 0;
    // Twice the line: the driver's figure adds the two free segments of its ring, and a write
    // needs one contiguous segment. With twice the room, one of the two always holds it.
    const size_t aligned = (length + 1 + 3) & ~size_t{3};
    if (length == 0 || length > kMaxLineLength ||
        uart_get_tx_buffer_free_size(port, &free_size) != ESP_OK || free_size < 2 * aligned) {
        return false;
    }
    // One write per line, with the CRLF of a terminal. Going through stdout would queue every
    // single byte as its own item in the driver's ring.
    char out[kMaxLineLength + 1];
    std::memcpy(out, line, length - 1);
    out[length - 1] = '\r';
    out[length] = '\n';
    return uart_write_bytes(port, out, length + 1) == static_cast<int>(length + 1);
}

}  // namespace logcat
