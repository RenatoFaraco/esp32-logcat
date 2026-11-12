#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/file_system.hpp"
#include "logcat/sink.hpp"

namespace logcat {

// Size limits of the store (ARCHITECTURE §5): 64 KB per file, 448 KB in all, which leaves
// room for LittleFS's own blocks in the 512 KB `logs` partition.
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
    bool begin();

    bool write(const char* line, size_t length) override;
    const char* name() const override { return "flash"; }

    // Makes what was written durable. false: it may not be.
    bool flush();

    StoreStats stats() const;

    // The files, oldest first (for the dump, #29). false past the end.
    size_t file_count() const { return count_; }
    bool file_name(size_t position, char (&out)[kStoreNameCapacity]) const;

private:
    struct Entry {
        uint32_t index;
        uint32_t size;
    };

    void scan();
    bool open_current();
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
    StoreStats stats_ = {};
};

}  // namespace logcat
