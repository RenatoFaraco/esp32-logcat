#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/rotating_store.hpp"

namespace logcat {

// One file of a dump, as it was when the dump started.
struct ArchiveFile {
    char name[kStoreNameCapacity];
    uint32_t size;  // bytes the dump sends: what the file had at the start (wire-format §3)
};

// The log in flash as `log dump` (#29) reads it: the port between the ReplyServer (core) and
// the flash store (an adapter). Used only by the writer task, the store's own task, so the
// store never sees two users at once (FileSystem's contract).
class LogArchive {
public:
    // Makes every byte written so far readable (a sync) and lists the files, oldest first,
    // with their sizes as of now. false: the sync failed, and the dump would miss lines.
    // end_dump() follows every call, whatever it returned.
    virtual bool begin_dump(ArchiveFile (&files)[kMaxStoreFiles], size_t& count) = 0;

    // Reads up to `length` bytes of a listed file from `offset`. Fewer than asked for within
    // the size listed: the file changed under the dump (the store removed or renamed it).
    virtual size_t read(const char* name, uint32_t offset, char* out, size_t length) = 0;

    // The dump is over: finished, failed or abandoned.
    virtual void end_dump() = 0;

protected:
    ~LogArchive() = default;  // never deleted through the interface: archives are statics
};

}  // namespace logcat
