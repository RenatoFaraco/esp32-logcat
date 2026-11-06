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
#include "sdkconfig.h"

// parse_idf_text understands log v1 lines with an RTOS millisecond timestamp. Other settings
// change what reaches the hook (log v2 splits a line into several calls; the system timestamp
// prints "hh:mm:ss.mmm") and every log would silently pass through raw: fail the build instead.
#if CONFIG_LOG_VERSION != 1 || !defined(CONFIG_LOG_TIMESTAMP_SOURCE_RTOS)
#error "IdfLogHook expects CONFIG_LOG_VERSION=1 and CONFIG_LOG_TIMESTAMP_SOURCE_RTOS"
#endif

namespace logcat {

namespace {

// The previous output (picolibc's vprintf by default). Atomic and read with a fallback: another
// task may log between esp_log_set_vprintf() installing the hook and the store of its result.
std::atomic<vprintf_like_t> previous_output{nullptr};
Sequencer* sequencer_ptr = nullptr;
TagLevelFilter* filter_ptr = nullptr;
std::atomic<uint32_t> last_cycles{0};
std::atomic<uint32_t> max_cycles{0};

// Per-task recursion guard: if anything on the output path logs (a driver reporting an error),
// that line is passed through raw instead of re-entering the hook. thread_local = one byte of
// FreeRTOS TLS per task.
thread_local bool in_hook = false;

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

struct HookGuard {
    HookGuard() { in_hook = true; }
    ~HookGuard() { in_hook = false; }
};

// Runs in the context of whoever logs, in any task, possibly several at once: it only touches
// its own stack, atomics and the lock-free Sequencer/TagLevelFilter. Never runs in an ISR, with
// the scheduler stopped or with the cache disabled: ESP-IDF detects those constrained contexts
// (esp_log_util_is_constrained) and prints through the ROM without calling the hook.
int hook(const char* format, va_list args) {
    if (in_hook) {
        return previous()(format, args);
    }
    const HookGuard guard;
    const uint32_t start = esp_cpu_get_cycle_count();

    // vsnprintf consumes `args`; keep a copy to pass non-log output through unchanged.
    va_list original;
    va_copy(original, args);
    // One buffer for the text and, later, for the formatted line: ~170 B on the caller's stack
    // instead of two. The text is dead once copied into the record.
    char buffer[kCaptureBufferSize];
    const int printed = vsnprintf(buffer, sizeof(buffer), format, args);
    CapturedLine captured{};
    if (!capture_idf_line(buffer, sizeof(buffer), printed, captured)) {
        const int written = previous()(format, original);
        va_end(original);
        return written;
    }
    va_end(original);

    LogRecord record{};
    record.level = captured.level;
    record.set_tag(captured.tag);
    // Filter before taking a seq: a filtered line must not look like a lost one on the host.
    if (!filter_ptr->enabled(record.tag, record.level)) {
        return 0;
    }
    record.set_message(captured.message);
    if (captured.cut) {
        record.truncated = true;  // shown as "..." only if the message is full (formatter rule)
    }
    record.uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    record.boot = 0;  // boot counter comes with NVS (M4)
    record.seq = sequencer_ptr->next();
    record_cycles(esp_cpu_get_cycle_count() - start);

    // PROVISIONAL (#10): synchronous output in the caller's context, until #11/#12. Measured:
    // when no host reads the native USB port, this blocks ~50 ms per line (see #12).
    const size_t length = format_line(record, buffer);
    return write_previous("%.*s", static_cast<int>(length), buffer);
}

}  // namespace

void install_idf_log_hook(Sequencer& sequencer, TagLevelFilter& filter) {
    sequencer_ptr = &sequencer;
    filter_ptr = &filter;
    const vprintf_like_t replaced = esp_log_set_vprintf(&hook);
    // A second call returns the hook itself: storing it would make the hook call itself forever.
    if (replaced != &hook) {
        previous_output.store(replaced, std::memory_order_release);
    }
}

CaptureStats idf_log_hook_stats() {
    return {last_cycles.load(std::memory_order_relaxed),
            max_cycles.load(std::memory_order_relaxed)};
}

}  // namespace logcat
