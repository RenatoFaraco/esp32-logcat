#include "logcat/flush_policy.hpp"

namespace logcat {

FlushPolicy::Reason FlushPolicy::after_line(LogLevel level, uint32_t now_ms, size_t unsynced) {
    if (level == LogLevel::Error) {
        error_pending_ = true;
    }
    return decide(now_ms, unsynced);
}

FlushPolicy::Reason FlushPolicy::on_idle(uint32_t now_ms, size_t unsynced) {
    return decide(now_ms, unsynced);
}

FlushPolicy::Reason FlushPolicy::decide(uint32_t now_ms, size_t unsynced) {
    if (unsynced == 0) {
        error_pending_ = false;  // nothing left that a sync could make durable
        return Reason::None;
    }
    const uint32_t since = now_ms - last_sync_;  // unsigned: right across the wrap
    if (error_pending_ && since >= kMinGapMs) {
        return Reason::Error;
    }
    return since >= kPeriodMs ? Reason::Period : Reason::None;
}

void FlushPolicy::synced(Reason reason, uint32_t now_ms, bool ok) {
    last_sync_ = now_ms;  // a failed sync too: a failing flash is not retried in a loop
    if (ok) {
        error_pending_ = false;  // every line written before this sync is durable now
    }
    switch (reason) {
        case Reason::Period:
            ++counts_.period;
            break;
        case Reason::Error:
            ++counts_.error;
            break;
        case Reason::Request:
            ++counts_.request;
            break;
        case Reason::None:
            break;
    }
}

}  // namespace logcat
