#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/file_system.hpp"
#include "logcat/sink.hpp"

namespace logcat {

// Size limits of the store (ARCHITECTURE §5): 64 KB per file, 448 KB in all, which leaves
// room for LittleFS's own blocks in the 512 KB `logs` partition.
// file_limit is clamped to total_limit. A line that does not fit the total even in an empty
// partition (total_limit < one line + 2) is always refused.
struct StoreConfig {
    uint32_t file_limit = 64 * 1024;
    uint32_t total_limit = 448 * 1024;
};

inline constexpr size_t kMaxStoreFiles = 16;      // with the default limits, 7 are ever kept
inline constexpr uint32_t kMaxFileIndex = 999;    // log_000.txt ... log_999.txt
inline constexpr size_t kStoreNameCapacity = 12;  // "log_000.txt" + '\0'

// "log_<index>.txt", three digits. index <= kMaxFileIndex.
void store_file_name(uint32_t index, char (&out)[kStoreNameCapacity]);

struct StoreStats {
    uint32_t files;           // log files now
    uint32_t bytes;           // their total size as the store counts it
    uint32_t rotations;       // new files started because the current one was full
    uint32_t removed;         // oldest files removed to stay under the total limit
    uint32_t repaired;        // torn tails closed with DEL + '\n' (wire-format §1)
    uint32_t failed_writes;   // lines that did not go in whole
    uint32_t failed_removes;  // oldest files that could not be removed (lines refused)
    uint32_t renumbered;      // times the files were renamed back to log_000 after log_999
    uint32_t sync_failures;   // flush() calls that did not make the data durable
    uint32_t lost_bytes;      // unsynced bytes dropped by a short write or a failed sync
};

// The log lines in flash (PRD F5): a Sink of the Writer that appends to the newest of
// log_000.txt ... log_999.txt, starts a new file when the current one is full, and removes the
// oldest so the files never add up to more than `total_limit`.
//
// Owned by the writer task, like every sink: no lock. stats() is meant for the same task.
//
// The rules that keep the files readable after any reset or power cut (#23):
// - every line goes in one write() call: never half a line from the store's side;
// - a file whose last byte is not '\n' (a cut inside a line, a short write) is closed with
//   DEL + '\n' before anything else is appended to it, so the parser classifies that piece as
//   torn and never glues it to the next record;
// - after a short write the file is closed and reopened before the next line (the port's
//   contract: the file is in error), which also repairs its tail.
// What reaches flash is decided by flush() (FileSystem::sync); when to call it is the flush
// policy's job (#26).
class RotatingStore final : public Sink {
public:
    explicit RotatingStore(FileSystem& fs, StoreConfig config = {});

    // At boot: finds the log files, removes the oldest beyond the limits, opens the newest
    // (creating log_000.txt on an empty partition) and repairs its tail. false: no file could
    // be opened; write() keeps trying.
    bool begin();  // write() before begin() refuses every line

    bool write(const char* line, size_t length) override;
    const char* name() const override { return "flash"; }

    // Makes what was written durable. true when nothing is pending (nothing open). false: it
    // may not be; the file is then closed (port contract: in error) and reopened, with its tail
    // repaired, before the next line.
    bool flush();

    bool is_open() const { return open_; }
    // Bytes written since the last sync or close: what a reset now would lose (#26 uses it to
    // skip empty syncs).
    uint32_t unsynced_bytes() const { return pending_; }

    StoreStats stats() const;

    // The files, oldest first (for the dump, #29). false past the end.
    size_t file_count() const { return count_; }
    bool file_name(size_t position, char (&out)[kStoreNameCapacity]) const;
    // The size the store counts for that file (including unsynced bytes of the open one: flush()
    // first for what a reader would see). 0 past the end.
    uint32_t file_size(size_t position) const {
        return position < count_ ? files_[position].size : 0;
    }

private:
    struct Entry {
        uint32_t index;
        uint32_t size;
        bool size_unknown;  // the scan could not read it: charged as full, repaired on open
    };

    void scan();
    bool open_current();
    bool reopen();
    bool repair();
    void lose_pending();
    bool rotate();
    bool renumber();
    bool make_room(size_t length);
    bool remove_oldest();
    void forget_oldest();
    uint32_t total() const;
    Entry& current() { return files_[count_ - 1]; }

    FileSystem& fs_;
    StoreConfig config_;
    Entry files_[kMaxStoreFiles] = {};
    size_t count_ = 0;
    bool open_ = false;
    bool needs_repair_ = false;
    uint32_t pending_ = 0;  // written since the last sync or close
    StoreStats stats_ = {};
};

}  // namespace logcat
