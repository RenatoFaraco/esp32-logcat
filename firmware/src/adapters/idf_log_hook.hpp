#pragma once

#include <cstdint>

#include "adapters/log_queue.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/tag_level_filter.hpp"

namespace logcat {

// Cost of the hook in the caller's context, in CPU cycles: vsnprintf + parse + building the
// record + the zero-timeout enqueue. Approximate on purpose: plain relaxed loads/stores, no
// read-modify-write. Wall-clock cycles: if the caller is preempted in the middle (time slicing
// with an equal-priority task, e.g. the writer during a burst), the other task's time is
// included. Measured on the board: typical ~15k cycles (~95 us at 160 MHz); a max of exactly
// one FreeRTOS tick (1.6M cycles, 10 ms) is such a preemption, not the hook's own cost.
struct CaptureStats {
    uint32_t last_cycles;
    uint32_t max_cycles;
};

// Routes every ESP_LOGx (application and ESP-IDF) through the logcat core: the formatted line is
// recognized (parse_idf_text), filtered per tag, numbered and put on `queue` with zero timeout.
// The writer task (writer_task.hpp) formats and writes it. Anything that is not a log line, and
// anything logged from inside the writer task, is passed to the previous output (the console)
// unchanged.
//
// Install it once, after the queue and the writer task exist, as early as possible in
// app_main; logs printed before keep the ESP-IDF format. An ESP_LOGx in an ISR or with the
// scheduler suspended (a bug: use ESP_DRAM_LOGx there) is printed raw by the ROM printer.
// `sequencer`, `filter` and `queue` must outlive the hook (use statics).
//
// Reentrant: called from any task, several at once (only stack, atomics, the lock-free core
// objects and the thread-safe queue). A log line never blocks: a full queue drops it and counts
// it (ADR-004). Cost for the caller: the text buffer and the LogRecord on its stack (~0.35 KB
// plus vsnprintf's own frame) and the capture cycles reported below.
//
// Passed through raw (and so possibly waiting on the console like any printf): text that is
// not a whole log line — vsnprintf errors, a line written in several calls — and anything
// logged inside the writer task. Ordinary ESP_LOGx lines never take this path.
void install_idf_log_hook(Sequencer& sequencer, TagLevelFilter& filter, FreeRtosLogQueue& queue);

CaptureStats idf_log_hook_stats();

}  // namespace logcat
