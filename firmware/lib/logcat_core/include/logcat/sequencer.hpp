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
class Sequencer {
public:
    explicit Sequencer(uint32_t first_seq = 0);

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
