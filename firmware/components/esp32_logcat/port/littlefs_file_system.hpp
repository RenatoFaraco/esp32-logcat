#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/file_system.hpp"

namespace logcat {

enum class MountResult : uint8_t {
    Mounted,    // an existing file system
    Formatted,  // none could be mounted: formatted, empty (see was_blank())
    Failed,     // no file system: every call fails
};

const char* to_string(MountResult result);

// The FileSystem port on esp_littlefs (joltwallet/littlefs), partition `logs` mounted at /logs
// (#24). What the port's contract says "to confirm on the board", as found in esp_littlefs
// 1.23 and by test/test_target_littlefs on the board:
// - stat() (size) reads the committed entry: the open file's unsynced bytes are not seen;
// - unlink/rename of an open file fail with EBUSY: refused here first, so LittleFS does not log
//   an error from inside the writer task;
// - rename replaces an existing target: checked here first, refused;
// - a full partition makes write() fail whole (NOSPC), never short: reported as 0 accepted.
// I/O goes through file descriptors (open/write/fsync), never stdio: fwrite would buffer, and a
// full partition would only show at fflush.
//
// Heap: esp_littlefs allocates a small entry on every open (and opendir), and read() opens the
// file each time. That is in the writer task, never in the caller of a log line (the rule of
// CLAUDE.md is about the latter); an allocation failure is an ordinary failed call. `stats`
// shows heap_min so a leak would be visible.
//
// Used only by the writer task (like the store), except begin() at boot, before it starts.
class LittleFsFileSystem final : public FileSystem {
public:
    static constexpr size_t kPathCapacity = 48;  // "/logs/" + a name, checked in the .cpp

    // Mounts the partition. A mount that fails is tried once more (a passing read glitch must not
    // cost the logs); if it still fails, the partition is formatted explicitly. Formatting is the
    // choice for availability: a device that cannot log is worse than one that lost old logs,
    // and was_blank() tells a first use from a real loss.
    MountResult begin();
    MountResult mount_result() const { return mount_; }
    // The partition was formatted because it held nothing at all (first use), not lost logs.
    bool was_blank() const { return was_blank_; }

    // What LittleFS itself uses on the partition (blocks, metadata included). false if unmounted.
    bool usage(size_t& used, size_t& total) const;

    bool open_append(const char* name) override;
    size_t write(const char* data, size_t length) override;
    bool sync() override;
    void close() override;
    int32_t size(const char* name) override;
    size_t read(const char* name, uint32_t offset, char* out, size_t length) override;
    bool remove(const char* name) override;
    bool rename(const char* from, const char* to) override;
    size_t list(NameVisitor& visitor) override;

private:
    bool path(const char* name, char (&out)[kPathCapacity]) const;
    bool is_open(const char* name) const;

    MountResult mount_ = MountResult::Failed;
    bool was_blank_ = false;
    int fd_ = -1;
    bool write_failed_ = false;  // the open file is in error (port contract)
    char open_name_[kMaxFileName + 1] = {};
};

}  // namespace logcat
