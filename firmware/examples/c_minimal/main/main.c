/*
 * esp32-logcat from plain C: start it, then log as usual with ESP_LOGx.
 * Read it on the PC with the esp-logcat host tool (`esp-logcat stream`, `esp-logcat shell`).
 */
#include "esp32_logcat.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MINIMAL";

void app_main(void)
{
    esp32_logcat_config_t config = ESP32_LOGCAT_CONFIG_DEFAULT();
    if (esp32_logcat_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "esp32_logcat_init failed: plain ESP-IDF logs");
    }

    /* Quiet a chatty tag at run time (the console's `log level` does the same). */
    esp32_logcat_set_level("CHATTY", ESP_LOG_WARN);

    for (int i = 1;; ++i) {
        ESP_LOGI(TAG, "hello from C, line %d", i);
        ESP_LOGI("CHATTY", "below WARN: filtered out");
        if (i % 10 == 0) {
            esp32_logcat_info_t info;
            esp32_logcat_get_info(&info);
            ESP_LOGW(TAG, "boot %lu, %lu lines dropped so far", (unsigned long)info.boot,
                     (unsigned long)info.dropped);
            /* Make what was logged so far durable before, say, a risky operation. */
            esp32_logcat_sync(1000);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
