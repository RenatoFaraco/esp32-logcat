#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/log_record.hpp"

namespace logcat {

// When the log in flash is synced (PRD F6, #26). Only decides; the caller (FlashStore, in the
// writer task) syncs and reports back with synced(). No clock of its own: `now_ms` is passed in
// (wrapping uint32 milliseconds, compared by difference).
//
// - every kPeriodMs, if bytes are pending: the loss window of a crash or power cut;
// - after an ERROR line, at once, but never sooner than kMinGapMs after the previous sync of
//   any kind: an ERROR is durable within a second, and an error storm costs at most one sync
//   (one 4 KB erase) a second;
// - on request (`log sync`, `reboot`): decided by the caller, only counted here.
//
// The period is checked after every line too, not only when the queue is empty: under heavy
// logging the queue never empties.
class FlushPolicy {
public:
    static constexpr uint32_t kPeriodMs = 5000;
    static constexpr uint32_t kMinGapMs = 1000;

    enum class Reason : uint8_t { None, Period, Error, Request };

    struct Counts {
        uint32_t period = 0;
        uint32_t error = 0;
        uint32_t request = 0;
    };

    explicit FlushPolicy(uint32_t now_ms) : last_sync_(now_ms) {}

    // After a line was written to the store. `unsynced`: the store's bytes not yet synced.
    Reason after_line(LogLevel level, uint32_t now_ms, size_t unsynced);
    // When the queue is empty.
    Reason on_idle(uint32_t now_ms, size_t unsynced);
    // A sync was made, for any reason; `ok` false: it failed (a pending ERROR stays owed).
    void synced(Reason reason, uint32_t now_ms, bool ok);

    Counts counts() const { return counts_; }

private:
    Reason decide(uint32_t now_ms, size_t unsynced);

    uint32_t last_sync_;
    bool error_pending_ = false;
    Counts counts_;
};

}  // namespace logcat
