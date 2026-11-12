#pragma once

#include <cstddef>
#include <cstdint>

namespace logcat {

// File names the core uses: short, without a directory (the adapter adds the mount point).
inline constexpr size_t kMaxFileName = 31;  // chars, without the '\0'

// Called once per file by FileSystem::list(). `name` is valid only during the call, and the
// visitor must not call the FileSystem: collect the names, then act on them.
class NameVisitor {
public:
    virtual void visit(const char* name) = 0;

protected:
    ~NameVisitor() = default;
};

// The port to a file system (ADR-003): LittleFS on the board (#24), a fake in memory for the
// tests (test/support/memory_fs.hpp). Only what the rotating store, the line reader and the
// dump need.
//
// The contract is the strict side of what LittleFS may do, so code that works on the fake works
// on the board (details to confirm on the board are listed for the #24 adapter below):
// - One file is open at a time, for appending. Opening another closes (and syncs) the first.
// - Durability: only sync() and close() put written bytes in flash. A power cut, panic or
//   watchdog loses what came after the last sync; LittleFS rolls the file back to it. A torn
//   last line is still possible: when a sync or close happens after only part of a line went
//   in (a short write), or on another file system.
// - size() and read() may not see the open file's bytes written since the last sync: call
//   sync() before taking a size or reading it (the dump's snapshot, #29).
// - After a short or failed write the open file is in error: what it took since the last sync
//   may be lost even if sync() or close() is called. Close it and open it again.
// - remove() and rename() refuse the open file (close it first); rename() refuses an existing
//   target.
// - Errors and "nothing" read the same: size() -1 = missing or error, read() 0 = end, missing
//   or error, list() 0 = empty or error.
// - Not thread-safe: used only by the writer task (the dump goes through it too).
// - No heap, no exceptions. Names are NUL-terminated and at most kMaxFileName chars.
//
// For the LittleFS adapter (#24), to confirm on the board:
// - write through open()/write()/fsync() on a file descriptor, not stdio: fwrite buffers, so a
//   full partition would only show at fflush, and the store needs one write per line;
// - LittleFS's rename replaces an existing target: check that it does not exist first;
// - check that unlink/rename of the open file is refused or close it before;
// - each read() by name opens and seeks: the line reader reads in blocks of 512 bytes or more;
// - static_assert that the mount point plus kMaxFileName fits CONFIG_LITTLEFS_OBJ_NAME_LEN.
class FileSystem {
public:
    // Opens `name` for appending, creating it if needed. false: not opened (no space, an error
    // of the file system, a bad name); nothing is open then.
    virtual bool open_append(const char* name) = 0;

    // Appends to the open file. Returns the bytes accepted: fewer than `length` (the partition
    // is full: see "in error" above) or 0 (nothing open, an error).
    virtual size_t write(const char* data, size_t length) = 0;

    // Makes what was written durable. false: it may not be (the data stays pending).
    virtual bool sync() = 0;

    // Closes the open file, if any, syncing it.
    virtual void close() = 0;

    // Size of `name` in bytes as last synced; -1 if missing or on error.
    virtual int32_t size(const char* name) = 0;

    // Reads up to `length` bytes of `name` from `offset`. Returns the bytes read: fewer at the
    // end of the file, 0 past it, if the file is missing, or on error.
    virtual size_t read(const char* name, uint32_t offset, char* out, size_t length) = 0;

    // false for the open file, a missing file, an error.
    virtual bool remove(const char* name) = 0;

    // Renames `from` to `to`. false if `to` exists, `from` is missing or open, or on error.
    virtual bool rename(const char* from, const char* to) = 0;

    // Calls `visitor` for each file, in no particular order. Returns how many there were.
    virtual size_t list(NameVisitor& visitor) = 0;

protected:
    ~FileSystem() = default;  // never deleted through the interface: file systems are statics
};

}  // namespace logcat
