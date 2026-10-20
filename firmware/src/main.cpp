#include "esp_log.h"

static constexpr const char* TAG = "APP";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "esp32-logcat hello");
}
