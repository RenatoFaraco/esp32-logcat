#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "logcat/console.hpp"
#include "logcat/flush_policy.hpp"
#include "logcat/log_archive.hpp"
#include "logcat/rotating_store.hpp"
#include "logcat/sink.hpp"
#include "port/littlefs_file_system.hpp"

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
// `log dump` and `log clear` (#29): the dump is served by the ReplyServer in the writer task,
// through the LogArchive side of this class; a clear is asked for by the console and done in
// the writer task, refused (`busy`) while a dump is being sent.
//
// Threads: begin() runs at boot, before the writer task. on_idle(), write() and the LogArchive
// calls run in the writer task. snapshot(), request_sync() and request_clear() are for other
// tasks (the console).
class FlashStore final : public Sink, public LogArchive {
public:
    // Publishes "not mounted" until begin(): a snapshot taken before it, or in a run with the
    // flash off, must not read as mounted (the zeroed slot would be MountResult::Mounted).
    FlashStore();

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

    // What a clear removed, counted by the writer task when it did it.
    struct Cleared {
        uint32_t files;
        uint32_t bytes;
    };

    // From another task (one at a time: the console): asks the writer task to remove every log
    // file, and waits. The writer does it once the queue is empty, so the lines logged before
    // the request go with the old files. A request not taken within `timeout` is withdrawn
    // (Failed, nothing removed): a late clear never happens after the answer. One taken is
    // waited for up to `taken_wait` more; past that the answer is Failed, and files may have
    // gone (a stuck writer task: the console must not hang).
    ClearResult request_clear(TickType_t timeout, TickType_t taken_wait, Cleared& cleared);

    // LogArchive, for the ReplyServer (writer task). The dump's sync counts as a request. Each
    // read opens the file: esp_littlefs allocates per open (in the writer task, not on the
    // path of whoever logs), and a read that fails reads short, as a removed file does.
    bool begin_dump(ArchiveFile (&files)[kMaxStoreFiles], size_t& count) override;
    size_t read(const char* name, uint32_t offset, char* out, size_t length) override;
    void end_dump() override { dumping_ = false; }

    FlashSnapshot snapshot() const;

    // LittleFS's own view of the partition, blocks and metadata included. It walks the file
    // system under LittleFS's lock: the writer task waits meanwhile (fine for a `stats`).
    bool usage(size_t& used, size_t& total) const { return fs_.usage(used, total); }

    // At boot, after begin(): what was found, for the boot line.
    bool tail_was_torn() const { return torn_at_boot_; }
    bool was_blank() const { return fs_.was_blank(); }

private:
    void publish();
    void serve_clear(bool queue_empty);
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
    // The clear request: Idle -> Requested (console) -> Taken -> Done (writer), or Requested ->
    // Idle when the console gives up. Whoever moves it out of Requested owns the outcome, so a
    // request is either withdrawn or done and answered, never both.
    enum ClearState : uint8_t { kClearIdle, kClearRequested, kClearTaken, kClearDone };
    std::atomic<uint8_t> clear_state_{kClearIdle};
    std::atomic<uint8_t> clear_result_{static_cast<uint8_t>(ClearResult::Failed)};
    std::atomic<uint32_t> cleared_files_{0};  // published with clear_result_
    std::atomic<uint32_t> cleared_bytes_{0};
    // The dump in progress (writer task only).
    bool dumping_ = false;
    uint32_t renumbered_at_dump_ = 0;
    // Published copy of the numbers (relaxed: they are statistics, each one consistent alone).
    std::atomic<uint32_t> published_[14] = {};
};

}  // namespace logcat
