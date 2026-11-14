#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "adapters/littlefs_file_system.hpp"
#include "freertos/FreeRTOS.h"
#include "logcat/rotating_store.hpp"

namespace logcat {

// What the console may read about the flash, published by the writer task (the store's only
// user) so that no other task touches the store itself.
struct FlashSnapshot {
    MountResult mount;
    StoreStats store;
    uint32_t limit;
    uint32_t unsynced;
};

// The log in flash on the board (#24): LittleFS on the `logs` partition, the RotatingStore as a
// sink of the Writer, and a simple periodic sync so that what was logged survives a reset.
// The full flush policy (at once on ERROR, on command, wear measured) is #26.
//
// Threads: begin() runs at boot, before the writer task. on_idle() and the sink run in the
// writer task. snapshot() and request_sync() are for other tasks (the console).
class FlashStore {
public:
    static constexpr TickType_t kSyncPeriod = pdMS_TO_TICKS(5000);

    // Mounts (formatting a blank or corrupt partition) and opens the newest log file.
    MountResult begin();

    Sink& sink() { return store_; }

    // In the writer task when its queue is empty: syncs if a request is pending, or if
    // kSyncPeriod passed with unsynced bytes; then publishes the numbers.
    void on_idle();

    // From another task: asks the writer task to sync, and waits up to `timeout` for it.
    // true when it was done.
    bool request_sync(TickType_t timeout);

    FlashSnapshot snapshot() const;

    // LittleFS's own view of the partition, blocks and metadata included.
    bool usage(size_t& used, size_t& total) const { return fs_.usage(used, total); }

    // At boot, after begin(): what was found, for the boot line.
    bool tail_was_torn() const { return torn_at_boot_; }

private:
    void publish();

    LittleFsFileSystem fs_;
    RotatingStore store_{fs_};
    TickType_t last_sync_ = 0;
    bool torn_at_boot_ = false;
    std::atomic<bool> sync_requested_{false};
    // Published copy of the numbers (relaxed: they are statistics, each one consistent alone).
    std::atomic<uint32_t> published_[11] = {};
};

}  // namespace logcat
