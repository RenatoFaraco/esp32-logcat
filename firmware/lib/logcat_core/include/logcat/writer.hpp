#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/clock.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/log_record.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/sink.hpp"

namespace logcat {

inline constexpr size_t kMaxSinks = 4;
inline constexpr char kWriterTag[] = "LOGCAT";

// The writer task's logic, without FreeRTOS: formats each record once and sends the line to
// every sink, and turns losses into log lines the host can see (ADR-004).
//
// Not thread-safe: owned by the single writer task. No heap; the line buffer is a member, not
// on the writer task's stack.
//
// Loss reports are W LOGCAT lines with their own seq (Sequencer::next()), emitted from idle().
// The writer task calls idle() when the queue is empty, so a report does not overtake queued
// lines. It can still overtake a line whose caller took its seq and was preempted before the
// enqueue, and, under sustained overload, the task forces idle() once a second with lines
// still queued: the host treats both as reordering (up to the queue depth), not as gaps.
//   - "dropped N lines: log queue full"   — Sequencer::take_dropped() (callers' side)
//   - "N lines not sent to usb: ..."      — a sink refused N lines (e.g. no host reading).
//     Reported only after that sink took a line again: while it keeps refusing, a report would
//     be refused too and only burn a seq.
// A sink that refuses a report keeps the count it explained (plus the report itself), so its
// next report covers the whole hole in seq.
class Writer {
public:
    // constexpr: a static Writer is constant-initialized, like Sequencer and TagLevelFilter.
    constexpr Writer(Sequencer& sequencer, Clock& clock) : sequencer_(sequencer), clock_(clock) {}

    // At most kMaxSinks; returns false when full. Call before the writer task starts.
    bool add_sink(Sink& sink);

    // Boot counter stamped on the writer's own lines (0 until NVS, M4). Call before the writer
    // task starts: afterwards it would race with the reports written by the task.
    void set_boot(uint32_t boot) { boot_ = boot; }

    void write(const LogRecord& record);

    // Call when the queue is empty: emits pending loss reports.
    void idle();

    uint32_t unsent(size_t sink_index) const;

private:
    struct SinkState {
        Sink* sink;
        uint32_t unsent;  // refused lines not reported yet
        bool last_ok;     // the last write to this sink succeeded
    };

    void send(size_t length);
    void report(LogLevel level, uint32_t count, const char* text_before, const char* text_after);

    Sequencer& sequencer_;
    Clock& clock_;
    SinkState sinks_[kMaxSinks] = {};
    size_t sink_count_ = 0;
    uint32_t boot_ = 0;
    char line_[kLineBufferSize] = {};
};

}  // namespace logcat
