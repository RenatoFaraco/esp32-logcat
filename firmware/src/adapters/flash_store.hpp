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
// Loss window until #26: a crash, watchdog or power cut loses what came after the last sync,
// up to kSyncPeriod plus about 1 s (the writer task reaches its idle path at least once a
// second under load), plus what is still in the 32-deep queue. ERROR lines included.
//
// Wear (estimated, to be measured by #26): after a sync, the next append copies LittleFS's
// partial block, about one 4 KB erase per sync with data. With the demo's 1 line/s heartbeat,
// ~17k erases a day over 128 blocks: roughly two years of a 100k-cycle flash.
//
// Threads: begin() runs at boot, before the writer task. on_idle() and the sink run in the
// writer task. snapshot() and request_sync() are for other tasks (the console).
class FlashStore {
public:
    static constexpr TickType_t kSyncPeriod = pdMS_TO_TICKS(5000);

    // Mounts (formatting when it cannot mount) and opens the newest log file.
    MountResult begin();

    Sink& sink() { return store_; }

    // In the writer task when its queue is empty: syncs if a request is pending, or if
    // kSyncPeriod passed with unsynced bytes; then publishes the numbers.
    void on_idle();

    // From another task: asks the writer task for a sync made after this call, and waits up to
    // `timeout`. true only if that sync happened and succeeded.
    bool request_sync(TickType_t timeout);

    FlashSnapshot snapshot() const;

    // LittleFS's own view of the partition, blocks and metadata included. It walks the file
    // system under LittleFS's lock: the writer task waits meanwhile (fine for a `stats`).
    bool usage(size_t& used, size_t& total) const { return fs_.usage(used, total); }

    // At boot, after begin(): what was found, for the boot line.
    bool tail_was_torn() const { return torn_at_boot_; }
    bool was_blank() const { return fs_.was_blank(); }

private:
    void publish();

    LittleFsFileSystem fs_;
    RotatingStore store_{fs_};
    TickType_t last_sync_ = 0;
    bool torn_at_boot_ = false;
    // Sync requests by generation: a request waits for a sync that started after it, so a late
    // answer to an older request is never taken for it.
    std::atomic<uint32_t> requested_{0};
    std::atomic<uint32_t> done_{0};
    std::atomic<bool> last_ok_{true};
    // Published copy of the numbers (relaxed: they are statistics, each one consistent alone).
    std::atomic<uint32_t> published_[11] = {};
};

}  // namespace logcat
