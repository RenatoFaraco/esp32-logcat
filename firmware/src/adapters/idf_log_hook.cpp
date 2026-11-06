#include "adapters/idf_log_hook.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "logcat/idf_text.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/log_record.hpp"

namespace logcat {

namespace {

// Room for "L (4294967295) " + a tag + ": " + a 127-char message. Lives on the caller's stack.
constexpr size_t kTextCapacity = 192;

// The previous output (normally newlib's vprintf). Atomic and read with a fallback: another task
// may log between esp_log_set_vprintf() installing the hook and the store below, and must not
// call a null function pointer.
std::atomic<vprintf_like_t> previous_output{nullptr};
Sequencer* sequencer_ptr = nullptr;
TagLevelFilter* filter_ptr = nullptr;
std::atomic<uint32_t> last_cycles{0};
std::atomic<uint32_t> max_cycles{0};

vprintf_like_t previous() {
    const vprintf_like_t output = previous_output.load(std::memory_order_acquire);
    return output != nullptr ? output : &vprintf;
}

int write_previous(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int written = previous()(format, args);
    va_end(args);
    return written;
}

void record_cycles(uint32_t cycles) {
    last_cycles.store(cycles, std::memory_order_relaxed);
    if (cycles > max_cycles.load(std::memory_order_relaxed)) {
        max_cycles.store(cycles, std::memory_order_relaxed);  // racy max: fine for a statistic
    }
}

// Runs in the context of whoever logs, in any task (never in an ISR: ESP_LOGx is not ISR-safe).
int hook(const char* format, va_list args) {
    const uint32_t start = esp_cpu_get_cycle_count();

    // vsnprintf consumes `args`; keep a copy to pass non-log output through unchanged.
    va_list original;
    va_copy(original, args);
    char text[kTextCapacity];
    const int printed = vsnprintf(text, sizeof(text), format, args);
    IdfText parsed{};
    if (printed < 0 ||
        !parse_idf_text(text,
                        static_cast<size_t>(printed) < sizeof(text) ? static_cast<size_t>(printed)
                                                                    : sizeof(text) - 1,
                        parsed)) {
        const int written = previous()(format, original);
        va_end(original);
        return written;
    }
    va_end(original);
    const bool cut = static_cast<size_t>(printed) >= sizeof(text);

    // The spans point into `text`, which we own: terminate them in place. The tag is followed by
    // ':' and the message by '\n', an ANSI reset or the end of the buffer.
    text[parsed.tag - text + parsed.tag_length] = '\0';
    text[parsed.message - text + parsed.message_length] = '\0';

    LogRecord record{};
    record.level = parsed.level;
    record.set_tag(parsed.tag);
    // Filter before taking a seq: a filtered line must not look like a lost one on the host.
    if (!filter_ptr->enabled(record.tag, record.level)) {
        return 0;
    }
    record.set_message(parsed.message);
    if (cut) {
        record.truncated = true;  // shown as "..." only if the message is full (formatter rule)
    }
    record.uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    record.boot = 0;  // boot counter comes with NVS (M4)
    record.seq = sequencer_ptr->next();
    record_cycles(esp_cpu_get_cycle_count() - start);

    // PROVISIONAL (#10): synchronous output in the caller's context, until #11/#12.
    char line[kLineBufferSize];
    const size_t length = format_line(record, line);
    return write_previous("%.*s", static_cast<int>(length), line);
}

}  // namespace

void install_idf_log_hook(Sequencer& sequencer, TagLevelFilter& filter) {
    sequencer_ptr = &sequencer;
    filter_ptr = &filter;
    previous_output.store(esp_log_set_vprintf(&hook), std::memory_order_release);
}

CaptureStats idf_log_hook_stats() {
    return {last_cycles.load(std::memory_order_relaxed),
            max_cycles.load(std::memory_order_relaxed)};
}

}  // namespace logcat
