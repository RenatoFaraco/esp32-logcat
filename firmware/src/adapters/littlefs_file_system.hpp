#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/file_system.hpp"

namespace logcat {

enum class MountResult : uint8_t {
    Mounted,    // an existing file system
    Formatted,  // none (or a corrupt one): formatted, empty
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
// Used only by the writer task (like the store), except begin() at boot, before it starts.
class LittleFsFileSystem final : public FileSystem {
public:
    // Mounts the partition, formatting it if it cannot be mounted.
    MountResult begin();
    MountResult mount_result() const { return mount_; }

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
    static constexpr size_t kPathCapacity = 6 + kMaxFileName + 1;  // "/logs/" + name + '\0'

    bool path(const char* name, char (&out)[kPathCapacity]) const;
    bool is_open(const char* name) const;

    MountResult mount_ = MountResult::Failed;
    int fd_ = -1;
    char open_name_[kMaxFileName + 1] = {};
};

}  // namespace logcat
