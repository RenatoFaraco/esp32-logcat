#include "adapters/flash_store.hpp"

#include "esp_timer.h"
#include "freertos/task.h"
#include "logcat/line_formatter.hpp"

namespace logcat {

namespace {

enum Slot : size_t {
    kFiles,
    kBytes,
    kRotations,
    kRemoved,
    kRepaired,
    kFailedWrites,
    kFailedRemoves,
    kSyncFailures,
    kLostBytes,
    kUnsynced,
    kSyncsPeriod,
    kSyncsError,
    kSyncsRequest,
    kMount,
};

}  // namespace

MountResult FlashStore::begin() {
    const MountResult mount = fs_.begin();
    if (mount != MountResult::Failed) {
        store_.begin();
        torn_at_boot_ = store_.tail_torn();
    }
    policy_ = FlushPolicy(now_ms());
    publish();
    return mount;
}

// esp_timer in ms, truncated to 32 bits: wraps after ~49.7 days, and the policy compares
// differences, which stay right across the wrap.
uint32_t FlashStore::now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool FlashStore::sync(FlushPolicy::Reason reason) {
    const bool ok = store_.flush();
    failures_ = store_.stats().sync_failures;
    policy_.synced(reason, now_ms(), ok);
    return ok;
}

bool FlashStore::write(const char* line, size_t length) {
    const uint32_t rotations = store_.stats().rotations;
    const bool taken = store_.write(line, length);
    const uint32_t now = now_ms();
    if (store_.stats().rotations != rotations) {
        // The rotation synced the old file: it counts as a sync for the 1-a-second limit. Told
        // before the line, which went to the new file and is not synced yet.
        policy_.synced(FlushPolicy::Reason::None, now, store_.stats().sync_failures == failures_);
        failures_ = store_.stats().sync_failures;
    }
    const FlushPolicy::Reason reason =
        policy_.after_line(line_level(line, length), now, store_.unsynced_bytes());
    // Only an overdue period syncs here. An ERROR waits for on_idle, which runs as soon as the
    // queue empties (at once on a quiet system) and at least once a second under load: syncing
    // here would stall this task for the erase while the queue fills, and drop the lines that
    // follow an error, the ones that explain it.
    if (reason == FlushPolicy::Reason::Period) {
        sync(reason);
        publish();
    }
    return taken;
}

void FlashStore::on_idle(bool queue_empty) {
    const uint32_t wanted = requested_.load(std::memory_order_acquire);
    // A request waits for an empty queue: lines logged before it may still be queued, and its
    // `ok` promises they are in flash. Under endless logging it times out (`failed`), never lies.
    if (queue_empty && wanted != done_.load(std::memory_order_relaxed)) {
        last_ok_.store(sync(FlushPolicy::Reason::Request), std::memory_order_relaxed);
        done_.store(wanted, std::memory_order_release);  // every request up to `wanted`
    } else {
        const FlushPolicy::Reason reason = policy_.on_idle(now_ms(), store_.unsynced_bytes());
        if (reason != FlushPolicy::Reason::None) {
            sync(reason);
        }
    }
    publish();
}

bool FlashStore::request_sync(TickType_t timeout) {
    // Lines the store lost since the request (a failed write or sync drops its pending bytes):
    // the sync after that would "succeed" over nothing, so the answer is no.
    const uint32_t losses_before = losses();
    const uint32_t mine = requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const TickType_t start = xTaskGetTickCount();
    // done_ catches up with requested_; wrap-safe comparison.
    while (static_cast<int32_t>(done_.load(std::memory_order_acquire) - mine) < 0) {
        if (xTaskGetTickCount() - start >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return last_ok_.load(std::memory_order_relaxed) && losses() == losses_before;
}

uint32_t FlashStore::losses() const {
    return published_[kFailedWrites].load(std::memory_order_relaxed) +
           published_[kLostBytes].load(std::memory_order_relaxed) +
           published_[kSyncFailures].load(std::memory_order_relaxed);
}

void FlashStore::publish() {
    const StoreStats stats = store_.stats();
    const FlushPolicy::Counts syncs = policy_.counts();
    const uint32_t values[] = {
        stats.files,          stats.bytes,
        stats.rotations,      stats.removed,
        stats.repaired,       stats.failed_writes,
        stats.failed_removes, stats.sync_failures,
        stats.lost_bytes,     store_.unsynced_bytes(),
        syncs.period,         syncs.error,
        syncs.request,        static_cast<uint32_t>(fs_.mount_result()),
    };
    static_assert(sizeof(values) / sizeof(values[0]) == kMount + 1, "one slot per value");
    static_assert(sizeof(published_) / sizeof(published_[0]) == kMount + 1,
                  "one published atomic per slot");
    for (size_t i = 0; i <= kMount; ++i) {
        published_[i].store(values[i], std::memory_order_relaxed);
    }
}

FlashSnapshot FlashStore::snapshot() const {
    FlashSnapshot out{};
    out.mount = static_cast<MountResult>(published_[kMount].load(std::memory_order_relaxed));
    out.store.files = published_[kFiles].load(std::memory_order_relaxed);
    out.store.bytes = published_[kBytes].load(std::memory_order_relaxed);
    out.store.rotations = published_[kRotations].load(std::memory_order_relaxed);
    out.store.removed = published_[kRemoved].load(std::memory_order_relaxed);
    out.store.repaired = published_[kRepaired].load(std::memory_order_relaxed);
    out.store.failed_writes = published_[kFailedWrites].load(std::memory_order_relaxed);
    out.store.failed_removes = published_[kFailedRemoves].load(std::memory_order_relaxed);
    out.store.sync_failures = published_[kSyncFailures].load(std::memory_order_relaxed);
    out.store.lost_bytes = published_[kLostBytes].load(std::memory_order_relaxed);
    out.unsynced = published_[kUnsynced].load(std::memory_order_relaxed);
    out.syncs.period = published_[kSyncsPeriod].load(std::memory_order_relaxed);
    out.syncs.error = published_[kSyncsError].load(std::memory_order_relaxed);
    out.syncs.request = published_[kSyncsRequest].load(std::memory_order_relaxed);
    out.limit = store_.config().total_limit;  // fixed after construction
    return out;
}

}  // namespace logcat
