#include <cstring>

#include "adapters/idf_log_hook.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/line_formatter.hpp"
#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"
#include "logcat/ring_buffer.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/tag_level_filter.hpp"

static constexpr const char* TAG = "APP";

// Constant-initialized (constexpr constructors): ready before app_main and the hook.
static logcat::Sequencer sequencer;
static logcat::TagLevelFilter tag_filter(logcat::LogLevel::Info);

// Continuous output to follow on a serial monitor (and later in `esp-logcat stream`). Also shows
// the hook's capture cost and how much of this task's stack is left after logging through it.
static void heartbeat_task(void*) {
    for (uint32_t tick = 1;; ++tick) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const logcat::CaptureStats stats = logcat::idf_log_hook_stats();
        ESP_LOGI("HEARTBEAT", "tick %lu, capture %lu cycles (max %lu), stack free %u B",
                 static_cast<unsigned long>(tick), static_cast<unsigned long>(stats.last_cycles),
                 static_cast<unsigned long>(stats.max_cycles),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
}

extern "C" void app_main(void) {
    // First thing: from here on every ESP_LOGx goes through the logcat core.
    logcat::install_idf_log_hook(sequencer, tag_filter);

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

    xTaskCreate(heartbeat_task, "heartbeat", 3072, nullptr, 1, nullptr);
}
