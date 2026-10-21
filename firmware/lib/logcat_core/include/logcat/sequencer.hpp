#pragma once

#include <atomic>
#include <cstdint>

namespace logcat {

// seq is printed with 6 digits (PRD §3), so it wraps to 0 after 999999. The host detects gaps
// with modular arithmetic.
inline constexpr uint32_t kSeqModulo = 1'000'000;

// Per-boot line numbering and dropped-line counter.
//
// seq is assigned in the caller's context, before the queue, so a dropped line leaves a gap
// the host can see (ADR-004). Thread safety: next() and record_drop() may be called
// concurrently from any task; take_dropped() is for the single writer task. Counters only
// carry their own value, so relaxed atomics are enough.
//
// Ordering: each seq is unique, but lines are not guaranteed to reach the writer in seq order.
// A task preempted between next() and the enqueue lets a later seq get written first, so the
// host must treat a small step back (up to the queue depth) as reordering, not as a gap.
// Call next() only after the level filter, otherwise filtered-out lines become false gaps.
class Sequencer {
public:
    // constexpr so a global instance is initialized at compile time: no static-init order
    // issue and no guard variable when logging starts before app_main.
    constexpr explicit Sequencer(uint32_t first_seq = 0)
        : next_seq_(first_seq % kSeqModulo), dropped_(0) {}

    // Returns the seq for a new line and advances, wrapping at kSeqModulo.
    uint32_t next();

    // Called when a line could not be enqueued.
    void record_drop();

    // Returns the lines dropped since the last call and resets the count.
    uint32_t take_dropped();

private:
    std::atomic<uint32_t> next_seq_;
    std::atomic<uint32_t> dropped_;
};

}  // namespace logcat
