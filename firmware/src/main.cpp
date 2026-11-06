#include <cstdio>

#include "esp_log.h"
#include "logcat/line_formatter.hpp"
#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"
#include "logcat/ring_buffer.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/tag_level_filter.hpp"

static constexpr const char* TAG = "APP";

// Constant-initialized (constexpr constructor): usable before app_main.
static logcat::TagLevelFilter tag_filter(logcat::LogLevel::Info);

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

    logcat::LogRecord parsed{};
    const logcat::ParseStatus status = logcat::parse_line(line, length, parsed);
    ESP_LOGI(TAG, "parsed back: %s (tag=%s)", logcat::to_string(status), parsed.tag);

    if (!tag_filter.set("NOISY", logcat::LogLevel::None)) {
        ESP_LOGW(TAG, "tag filter table full");
    }
    static logcat::RingBuffer<logcat::LogRecord, 4> recent;  // zeroed by the loader: empty
    if (tag_filter.enabled(parsed.tag, parsed.level)) {
        recent.push(parsed);
    }
    ESP_LOGI(TAG, "ring buffer: %u/%u, consistent=%d", static_cast<unsigned>(recent.size()),
             static_cast<unsigned>(recent.capacity()), recent.is_consistent());
}
