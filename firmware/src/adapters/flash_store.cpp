#include "adapters/flash_store.hpp"

#include "freertos/task.h"

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
    kMount,
};

}  // namespace

MountResult FlashStore::begin() {
    const MountResult mount = fs_.begin();
    if (mount != MountResult::Failed) {
        store_.begin();
        torn_at_boot_ = store_.tail_torn();
    }
    last_sync_ = xTaskGetTickCount();
    publish();
    return mount;
}

void FlashStore::on_idle() {
    const TickType_t now = xTaskGetTickCount();
    const uint32_t wanted = requested_.load(std::memory_order_acquire);
    const bool requested = wanted != done_.load(std::memory_order_relaxed);
    if (requested || (store_.unsynced_bytes() > 0 && now - last_sync_ >= kSyncPeriod)) {
        const bool ok = store_.flush();
        last_sync_ = now;
        if (requested) {
            last_ok_.store(ok, std::memory_order_relaxed);
            done_.store(wanted, std::memory_order_release);  // every request up to `wanted`
        }
    }
    publish();
}

bool FlashStore::request_sync(TickType_t timeout) {
    const uint32_t mine = requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const TickType_t start = xTaskGetTickCount();
    // done_ catches up with requested_; wrap-safe comparison.
    while (static_cast<int32_t>(done_.load(std::memory_order_acquire) - mine) < 0) {
        if (xTaskGetTickCount() - start >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return last_ok_.load(std::memory_order_relaxed);
}

void FlashStore::publish() {
    const StoreStats stats = store_.stats();
    const uint32_t values[] = {
        stats.files,
        stats.bytes,
        stats.rotations,
        stats.removed,
        stats.repaired,
        stats.failed_writes,
        stats.failed_removes,
        stats.sync_failures,
        stats.lost_bytes,
        store_.unsynced_bytes(),
        static_cast<uint32_t>(fs_.mount_result()),
    };
    static_assert(sizeof(values) / sizeof(values[0]) == kMount + 1, "one slot per value");
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
    out.limit = store_.config().total_limit;  // fixed after construction
    return out;
}

}  // namespace logcat
