#include "adapters/idf_log_hook.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "adapters/writer_task.hpp"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/idf_text.hpp"
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
FreeRtosLogQueue* queue_ptr = nullptr;
std::atomic<uint32_t> last_cycles{0};
std::atomic<uint32_t> max_cycles{0};
std::atomic<uint32_t> dropped_total{0};  // never reset: readers take differences

// Per-task recursion guard: if anything on the output path logs (a driver reporting an error),
// that line is passed through raw instead of re-entering the hook. thread_local = one byte of
// FreeRTOS TLS per task.
thread_local bool in_hook = false;

vprintf_like_t previous() {
    const vprintf_like_t output = previous_output.load(std::memory_order_acquire);
    return output != nullptr ? output : &vprintf;
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
// its own stack, atomics, the lock-free Sequencer/TagLevelFilter and the queue.
//
// Log v1 does NOT keep constrained contexts away from the hook (only log v2 with
// CONFIG_LOG_API_CONSTRAINED_ENV_SAFE routes them to the ROM): an ESP_LOGx wrongly placed in an
// ISR, or with the scheduler suspended, reaches this function. There, xQueueSend and the
// stdout lock are illegal, so such text goes to the ROM printer (no locks, no queue).
int hook(const char* format, va_list args) {
    if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return esp_rom_vprintf(format, args);
    }
    // Inside the writer task (a driver logging while we write to it): straight to the console,
    // raw. Going through the queue would loop (write -> log -> enqueue -> write ...), and the
    // USB sink may be the very thing that is failing. Through the ROM printer, not stdout:
    // stdout queues each byte in the UART driver, whose full ring busy-waits this very task.
    if (xTaskGetCurrentTaskHandle() == writer_task_handle()) {
        return esp_rom_vprintf(format, args);
    }
    if (in_hook) {
        return previous()(format, args);
    }
    const HookGuard guard;
    const uint32_t start = esp_cpu_get_cycle_count();

    // vsnprintf consumes `args`; keep a copy to pass non-log output through unchanged.
    va_list original;
    va_copy(original, args);
    // The formatted ESP-IDF text, ~170 B on the caller's stack; dead once copied into the
    // record (the line itself is formatted later, by the writer task).
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
    // Zero timeout: a full queue drops the line and counts it; the writer reports the count
    // and the host sees the gap in seq (ADR-004). The caller never waits for I/O.
    if (!queue_ptr->try_send(record)) {
        sequencer_ptr->record_drop();
        dropped_total.fetch_add(1, std::memory_order_relaxed);
    }
    record_cycles(esp_cpu_get_cycle_count() - start);
    return 0;
}

}  // namespace

void install_idf_log_hook(Sequencer& sequencer, TagLevelFilter& filter, FreeRtosLogQueue& queue) {
    sequencer_ptr = &sequencer;
    filter_ptr = &filter;
    queue_ptr = &queue;
    const vprintf_like_t replaced = esp_log_set_vprintf(&hook);
    // A second call returns the hook itself: storing it would make the hook call itself forever.
    if (replaced != &hook) {
        previous_output.store(replaced, std::memory_order_release);
    }
}

CaptureStats idf_log_hook_stats() {
    return {last_cycles.load(std::memory_order_relaxed), max_cycles.load(std::memory_order_relaxed),
            dropped_total.load(std::memory_order_relaxed)};
}

}  // namespace logcat
