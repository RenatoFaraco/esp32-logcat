// The demo app: esp32-logcat through its public C API only (esp32_logcat.h), plus what makes
// it a demo: a burst that fills the queue, a silenced tag, a heartbeat, and the timing sweep
// (CONFIG_LOGCAT_DEMO_TIMING_SWEEP).

#include <cstring>

#include "app/timing_sweep.hpp"
#include "esp32_logcat.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static constexpr const char* TAG = "APP";

// Continuous output to follow in `esp-logcat stream`. Also shows the hook's cost in the caller
// (capture + enqueue) and the stack left in this task and in the writer task.
static void heartbeat_task(void*) {
    for (uint32_t tick = 1;; ++tick) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp32_logcat_info_t info{};
        esp32_logcat_get_info(&info);
        ESP_LOGI(
            "HEARTBEAT", "tick %lu, capture %lu cycles (max %lu), stack free %u B, writer %lu B",
            static_cast<unsigned long>(tick), static_cast<unsigned long>(info.capture_cycles_last),
            static_cast<unsigned long>(info.capture_cycles_max),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
            static_cast<unsigned long>(info.writer_stack_free));
        ESP_LOGD("HEARTBEAT", "debug detail of tick %lu: shown after `log level HEARTBEAT D`",
                 static_cast<unsigned long>(tick));
    }
}

extern "C" void app_main(void) {
    // Everything: boot counter, outputs, the log in flash, writer task, hook, console. From
    // here on every ESP_LOGx line goes through the logcat.
    if (esp32_logcat_init(nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "esp32_logcat_init failed: plain ESP-IDF logs");
    }

    ESP_LOGI(TAG, "esp32-logcat hook installed");
    ESP_LOGW(TAG, "a warning, letter W");
    ESP_LOGE(TAG, "an error, letter E");
    ESP_LOGD(TAG, "debug: filtered by ESP-IDF itself (default level INFO), never reaches us");

    if (esp32_logcat_set_level("NOISY", ESP_LOG_NONE) != ESP_OK) {
        ESP_LOGW(TAG, "could not silence NOISY");
    }
    ESP_LOGI("NOISY", "silenced by the tag filter: this line must not appear");

    char long_message[200];
    std::memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    ESP_LOGI(TAG, "%s", long_message);  // cut to 124 chars + "..."

    // Burst: 100 lines in a few ms, faster than the writer task gets to run (same priority as
    // this one). The 32-deep queue fills, the rest is dropped and counted,
    // and the writer reports "dropped N lines: log queue full"; the host shows the gap (#11).
    for (int i = 1; i <= 100; ++i) {
        ESP_LOGI("BURST", "line %d of 100", i);
    }

    if (xTaskCreate(heartbeat_task, "heartbeat", 3072, nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "heartbeat task not created (out of heap)");
    }

#if CONFIG_LOGCAT_DEMO_TIMING_SWEEP
    // 1 kHz critical task + log load in phases, for the timing criterion (#13, #14). Only with
    // CONFIG_LOGCAT_DEMO_TIMING_SWEEP (sdkconfig.timing): it writes ~2 MB to flash, which would
    // push the logs of the previous boot (the ones a crash left) out of the 448 KB, and wear the
    // flash.
    vTaskDelay(pdMS_TO_TICKS(2000));  // let the burst drain and a host connect
    if (!app::start_timing_sweep()) {
        ESP_LOGE(TAG, "timing sweep not started");
    }
#endif
}
