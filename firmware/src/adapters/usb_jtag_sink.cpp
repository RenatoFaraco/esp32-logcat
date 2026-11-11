#include "adapters/usb_jtag_sink.hpp"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if !defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE)
#error "UsbJtagSink needs CONFIG_ESP_CONSOLE_SECONDARY_NONE=y (see sdkconfig.defaults)"
#endif

namespace logcat {

esp_err_t UsbJtagSink::begin() {
    if (usb_serial_jtag_is_driver_installed()) {
        return ESP_OK;
    }
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.tx_buffer_size = kTxBufferSize;
    config.rx_buffer_size = kRxBufferSize;
    return usb_serial_jtag_driver_install(&config);
}

bool UsbJtagSink::write(const char* line, size_t length) {
    // Zero ticks: the driver's TX mutex and ring buffer are tried once. Partial writes cannot
    // happen with a byte ring buffer, but anything short of the whole line counts as refused.
    const int written = usb_serial_jtag_write_bytes(line, length, 0);
    return written == static_cast<int>(length);
}

bool UsbJtagSink::write_reply(const char* line, size_t length) {
    constexpr TickType_t kReplyWait = pdMS_TO_TICKS(50);
    const int written = usb_serial_jtag_write_bytes(line, length, kReplyWait);
    return written == static_cast<int>(length);
}

}  // namespace logcat
