#include <cstdio>

#include "esp_log.h"
#include "logcat/line_formatter.hpp"
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

    static char line[logcat::kLineBufferSize];
    const size_t length = logcat::format_line(record, line);
    ESP_LOGI(TAG, "formatted %u bytes, LogRecord=%u bytes", static_cast<unsigned>(length),
             static_cast<unsigned>(sizeof(record)));
    fwrite(line, 1, length, stdout);
}
