#include "adapters/console_sink.hpp"

#include <cstdio>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
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
    const size_t written = std::fwrite(line, 1, length, stdout);
    std::fflush(stdout);
    return written == length;
}

}  // namespace logcat
