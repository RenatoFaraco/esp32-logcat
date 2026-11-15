#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "adapters/littlefs_file_system.hpp"
#include "freertos/FreeRTOS.h"
#include "logcat/flush_policy.hpp"
#include "logcat/rotating_store.hpp"
#include "logcat/sink.hpp"

namespace logcat {

// What the console may read about the flash, published by the writer task (the store's only
// user) so that no other task touches the store itself.
struct FlashSnapshot {
    MountResult mount;
    StoreStats store;
    uint32_t limit;
    uint32_t unsynced;
    FlushPolicy::Counts syncs;  // since boot, by reason
};

// The log in flash on the board (#24): LittleFS on the `logs` partition, the RotatingStore
// behind this sink of the Writer, synced by the FlushPolicy (#26).
//
// When a sync happens (FlushPolicy): every 5 s with pending bytes, checked after every line
// and when the queue empties, so heavy logging cannot hold it off; after an ERROR line, as soon
// as the queue empties (at once on a quiet system), at most one sync a second and within ~2 s
// under load; on request (`log sync`, `reboot`), once every line logged before it is written.
// Loss window of a crash, watchdog or power cut: what came after the last sync, at most 5 s
// (an ERROR line: 1 s quiet, ~2 s under load), plus what was still in the 32-deep queue.
//
// A sync blocks the writer task for a few ms (LittleFS copies its partial block: about one
// 4 KB erase, stretched by flash auto suspend). The queue holds the lines meanwhile.
//
// Threads: begin() runs at boot, before the writer task. on_idle() and write() run in the
// writer task. snapshot() and request_sync() are for other tasks (the console).
class FlashStore final : public Sink {
public:
    // Mounts (formatting when it cannot mount) and opens the newest log file.
    MountResult begin();

    Sink& sink() { return *this; }

    // The Writer's sink: the line goes to the store, then the policy may sync at once.
    bool write(const char* line, size_t length) override;
    const char* name() const override { return store_.name(); }

    // In the writer task (writer_task.hpp): a requested sync, once the queue is empty so that
    // every line logged before the request is in the store; otherwise what the policy says.
    // Then publishes the numbers.
    void on_idle(bool queue_empty);

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
    bool sync(FlushPolicy::Reason reason);
    static uint32_t now_ms();
    uint32_t losses() const;  // from the published numbers: any task

    LittleFsFileSystem fs_;
    RotatingStore store_{fs_};
    FlushPolicy policy_{0};  // restarted at begin() with the real time
    uint32_t failures_ = 0;  // the store's sync failures as of the last look (writer task)
    bool torn_at_boot_ = false;
    // Sync requests by generation: a request waits for a sync that started after it, so a late
    // answer to an older request is never taken for it.
    std::atomic<uint32_t> requested_{0};
    std::atomic<uint32_t> done_{0};
    std::atomic<bool> last_ok_{true};
    // Published copy of the numbers (relaxed: they are statistics, each one consistent alone).
    std::atomic<uint32_t> published_[14] = {};
};

}  // namespace logcat
