#pragma once

#include <cstddef>
#include <cstdint>

namespace logcat {

// File names the core uses: short, without a directory (the adapter adds the mount point).
inline constexpr size_t kMaxFileName = 31;  // chars, without the '\0'

// Called once per file by FileSystem::list(). `name` is valid only during the call.
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
// One file is open at a time, for appending: the store writes to one file only. Opening
// another closes the previous one. Reading, size, remove, rename and list work by name and do
// not need the file to be open (on LittleFS, size() of the open file includes what was
// written but not yet synced).
//
// Durability: what write() returns is accepted, not stored. Only sync() (or close()) puts it
// in flash; a power cut loses what came after the last sync, and may keep part of it: the end
// of a file can be torn anywhere (the store repairs that at the next open, #23).
//
// No heap, no exceptions: failures are return values. Names are NUL-terminated and at most
// kMaxFileName chars.
class FileSystem {
public:
    // Opens `name` for appending, creating it if needed. false: not opened (no space, an error
    // of the file system, a bad name); nothing is open then.
    virtual bool open_append(const char* name) = 0;

    // Appends to the open file. Returns the bytes accepted, which may be fewer than `length`
    // (the partition is full) or 0 (nothing open, an error).
    virtual size_t write(const char* data, size_t length) = 0;

    // Makes what was written durable. false: it may not be.
    virtual bool sync() = 0;

    // Closes the open file, if any (and syncs it).
    virtual void close() = 0;

    // Size of `name` in bytes, -1 if it does not exist.
    virtual int32_t size(const char* name) = 0;

    // Reads up to `length` bytes of `name` from `offset`. Returns the bytes read: fewer at the
    // end of the file, 0 past it or if the file does not exist.
    virtual size_t read(const char* name, uint32_t offset, char* out, size_t length) = 0;

    virtual bool remove(const char* name) = 0;

    // Renames `from` to `to`; fails if `to` exists.
    virtual bool rename(const char* from, const char* to) = 0;

    // Calls `visitor` for each file, in no particular order. Returns how many there were.
    virtual size_t list(NameVisitor& visitor) = 0;

protected:
    ~FileSystem() = default;  // never deleted through the interface: file systems are statics
};

}  // namespace logcat
