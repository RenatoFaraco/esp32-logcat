#include <cstring>

#include "adapters/console_sink.hpp"
#include "adapters/esp_clock.hpp"
#include "adapters/idf_log_hook.hpp"
#include "adapters/log_queue.hpp"
#include "adapters/usb_jtag_sink.hpp"
#include "adapters/writer_task.hpp"
#include "app/switchable_sink.hpp"
#include "app/timing_sweep.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/line_formatter.hpp"
#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"
#include "logcat/ring_buffer.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/tag_level_filter.hpp"
#include "logcat/writer.hpp"

static constexpr const char* TAG = "APP";

// Constant-initialized (constexpr constructors): ready before app_main and the hook.
static logcat::Sequencer sequencer;
static logcat::TagLevelFilter tag_filter(logcat::LogLevel::Info);
// The logging pipeline: callers -> queue -> writer task -> sinks. All statically allocated.
static logcat::FreeRtosLogQueue log_queue;
static logcat::EspClock uptime_clock;
static logcat::Writer writer(sequencer, uptime_clock);
static logcat::UsbJtagSink usb_sink;      // native USB port: the logcat stream
static logcat::ConsoleSink console_sink;  // UART console (CH340K): mirror for monitors
// The timing sweep turns the mirror off: the UART drains ~70 lines/s, the demo logs 1000/s.
static app::SwitchableSink uart_mirror(console_sink);

// Continuous output to follow in `esp-logcat stream`. Also shows the hook's cost in the caller
// (capture + enqueue) and the stack left in this task and in the writer task.
static void heartbeat_task(void*) {
    for (uint32_t tick = 1;; ++tick) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const logcat::CaptureStats stats = logcat::idf_log_hook_stats();
        ESP_LOGI("HEARTBEAT",
                 "tick %lu, capture %lu cycles (max %lu), stack free %u B, writer %u B",
                 static_cast<unsigned long>(tick), static_cast<unsigned long>(stats.last_cycles),
                 static_cast<unsigned long>(stats.max_cycles),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(logcat::writer_task_handle())));
    }
}

extern "C" void app_main(void) {
    // Sinks, writer task, then the hook: from there on every ESP_LOGx goes through the core.
    // Anything logged before (the driver install below) keeps the ESP-IDF format, on UART.
    if (usb_sink.begin() == ESP_OK) {
        writer.add_sink(usb_sink);
    } else {
        ESP_LOGE(TAG, "usb_serial_jtag driver install failed: logs only on the console");
    }
    if (console_sink.begin() != ESP_OK) {
        ESP_LOGE(TAG, "uart driver install failed: console mirror busy-waits on the FIFO");
    }
    writer.add_sink(uart_mirror);
    log_queue.begin();
    if (logcat::start_writer_task(writer, log_queue) == nullptr) {
        ESP_LOGE(TAG, "writer task not created: logs stay in the ESP-IDF format");
        return;
    }
    logcat::install_idf_log_hook(sequencer, tag_filter, log_queue);

    ESP_LOGI(TAG, "esp32-logcat hook installed");
    ESP_LOGW(TAG, "a warning, letter W");
    ESP_LOGE(TAG, "an error, letter E");
    ESP_LOGD(TAG, "debug: filtered by ESP-IDF itself (default level INFO), never reaches us");

    if (!tag_filter.set("NOISY", logcat::LogLevel::None)) {
        ESP_LOGW(TAG, "tag filter table full");
    }
    ESP_LOGI("NOISY", "silenced by TagLevelFilter: this line must not appear");

    char long_message[200];
    std::memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    ESP_LOGI(TAG, "%s", long_message);  // cut to 124 chars + "..."

    // The other core pieces, still exercised so the target build compiles them (ADR-005).
    logcat::LogRecord record{};
    record.level = logcat::LogLevel::Info;
    record.set_tag(TAG);
    record.set_message("round trip on the device");
    char line[logcat::kLineBufferSize];
    const size_t length = logcat::format_line(record, line);
    logcat::LogRecord parsed{};
    const logcat::ParseStatus status = logcat::parse_line(line, length, parsed);
    static logcat::RingBuffer<logcat::LogRecord, 4> recent;  // zeroed by the loader: empty
    recent.push(parsed);
    ESP_LOGI(TAG, "parse_line on the device: %s, ring buffer %u/%u", logcat::to_string(status),
             static_cast<unsigned>(recent.size()), static_cast<unsigned>(recent.capacity()));

    // Burst: 100 lines in a few ms, faster than the writer drains them (the console mirror
    // alone takes ~15 ms per line). The 32-deep queue fills, the rest is dropped and counted,
    // and the writer reports "dropped N lines: log queue full"; the host shows the gap (#11).
    for (int i = 1; i <= 100; ++i) {
        ESP_LOGI("BURST", "line %d of 100", i);
    }

    if (xTaskCreate(heartbeat_task, "heartbeat", 3072, nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "heartbeat task not created (out of heap)");
    }

    // 1 kHz critical task + log load in phases, for the timing criterion (#13, #14).
    vTaskDelay(pdMS_TO_TICKS(2000));  // let the burst drain and a host connect
    if (!app::start_timing_sweep(uart_mirror)) {
        ESP_LOGE(TAG, "timing sweep not started");
    }
}
