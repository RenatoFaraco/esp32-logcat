#include <cinttypes>

#include "esp_log.h"
#include "logcat/log_record.hpp"
#include "logcat/sequencer.hpp"

static constexpr const char* TAG = "APP";

extern "C" void app_main(void) {
    // Placeholder until M2: uses the core so the target build compiles it (ADR-005).
    static logcat::Sequencer sequencer;
    logcat::LogRecord record{};
    record.seq = sequencer.next();
    record.level = logcat::LogLevel::Info;
    record.set_tag(TAG);
    record.set_message("esp32-logcat hello");

    ESP_LOGI(TAG, "%s (seq=%" PRIu32 ", LogRecord=%u bytes)", record.msg, record.seq,
             static_cast<unsigned>(sizeof(record)));
}
