#include "adapters/console_task.hpp"

#include <cstdio>
#include <cstdlib>

#include "adapters/idf_log_hook.hpp"
#include "adapters/writer_task.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

namespace logcat {

namespace {

constexpr const char* TAG = "CONSOLE";
constexpr uint32_t kStackBytes = 4096;  // ESP-IDF FreeRTOS: stack depth in bytes
constexpr UBaseType_t kPriority = 1;
constexpr TickType_t kReadWait = pdMS_TO_TICKS(100);
// The console may wait for room, but not past the host's own 2 s timeout (ADR-012 §6).
constexpr TickType_t kSendWait = pdMS_TO_TICKS(500);
// Long enough for the writer task to take what is queued and the driver to send it.
constexpr TickType_t kDrainWait = pdMS_TO_TICKS(100);
constexpr int kDrainPolls = 100;  // of 10 ms
constexpr uint32_t kProtocolVersion = 1;

StackType_t stack[kStackBytes];
StaticTask_t control;
Console* console_ptr = nullptr;

// nullptr for ESP_RST_UNKNOWN: a reason nobody knows is left out, never invented (ADR-012 §2).
const char* reset_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "POWERON";
        case ESP_RST_EXT:
            return "EXT";
        case ESP_RST_SW:
            return "SW";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT_WDT";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT";
        case ESP_RST_WDT:
            return "WDT";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "SDIO";
        case ESP_RST_USB:
            return "USB";
        case ESP_RST_JTAG:
            return "JTAG";
        default:
            return nullptr;
    }
}

void run(void*) {
    char buffer[64];
    for (;;) {
        const int received = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), kReadWait);
        if (received > 0) {
            console_ptr->feed(buffer, static_cast<size_t>(received));
        }
    }
}

}  // namespace

bool EspConsolePlatform::send(const ReplyMessage& message) {
    const bool sent = replies_.send(message, kSendWait);
    queue_timeouts_ += sent ? 0 : 1;
    return sent;
}

void EspConsolePlatform::fill_info(ReplyFields& fields) {
    fields.add("fw", esp_app_get_description()->version);
    fields.add_uint("proto", kProtocolVersion);
    fields.add("chip", CONFIG_IDF_TARGET);
    fields.add("idf", esp_get_idf_version());
    fields.add_uint("uptime_ms", static_cast<uint32_t>(esp_timer_get_time() / 1000));
    const char* reset = reset_name(esp_reset_reason());
    if (reset != nullptr) {
        fields.add("reset", reset);
    }
}

void EspConsolePlatform::fill_stats(ReplyFields& fields) {
    const CaptureStats capture = idf_log_hook_stats();
    char pair[24];
    fields.add_uint("dropped", capture.dropped_total);
    std::snprintf(pair, sizeof(pair), "%u/%u", static_cast<unsigned>(log_queue_.used()),
                  static_cast<unsigned>(FreeRtosLogQueue::kDepth));
    fields.add("queue", pair);
    fields.add_uint("queue_max", log_queue_.high_water());
    std::snprintf(pair, sizeof(pair), "%lu/%lu", static_cast<unsigned long>(capture.last_cycles),
                  static_cast<unsigned long>(capture.max_cycles));
    fields.add("capture_cycles", pair);
    fields.add_uint("writer_stack_free", uxTaskGetStackHighWaterMark(writer_task_handle()));
    fields.add_uint("console_stack_free", uxTaskGetStackHighWaterMark(nullptr));
    for (size_t i = 0; i < writer_.sink_count(); ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "not_sent.%s", writer_.sink_name(i));
        fields.add_uint(key, writer_.refused_total(i));
    }
    fields.add_uint("tail_skipped", tail_.skipped());
    fields.add_uint("reply_failed", server_.failures() + queue_timeouts_);
}

void EspConsolePlatform::set_log_level(const char* tag, LogLevel level) {
    // ESP-IDF drops a line by its own level before the hook sees it, so a D or V line needs
    // this gate open too. Numbers match (see log_record.hpp). Opening only the tag asked for
    // keeps the other ESP_LOGD of the IDF out of the hook. `log level * D` does open them all.
    // A tag cut to 15 chars does not match the full name here: it cannot go above the default.
    esp_log_level_set(tag, static_cast<esp_log_level_t>(level));
}

void EspConsolePlatform::reboot() {
    // Bounded: a stuck writer task is when a reboot is needed most. Under heavy logging the log
    // queue may never empty, and what is left in it at the end is lost.
    for (int i = 0; i < kDrainPolls && !(replies_.empty() && log_queue_.empty()); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(kDrainWait);
    usb_serial_jtag_wait_tx_done(kDrainWait);
    esp_restart();
}

void EspConsolePlatform::crash_panic() {
    ESP_LOGE(TAG, "crash panic requested");
    vTaskDelay(kDrainWait);  // the line above reaches the host and the sinks
    abort();
}

void EspConsolePlatform::crash_wdt() {
    ESP_LOGE(TAG, "crash wdt requested");
    vTaskDelay(kDrainWait);
    portDISABLE_INTERRUPTS();  // no tick, no task switch: the interrupt watchdog fires
    for (;;) {
    }
}

TaskHandle_t start_console_task(Console& console) {
    console_ptr = &console;
    return xTaskCreateStatic(run, "logcat_console", kStackBytes, nullptr, kPriority, stack,
                             &control);
}

}  // namespace logcat
