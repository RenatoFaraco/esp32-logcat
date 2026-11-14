#include "adapters/littlefs_file_system.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "esp_littlefs.h"
#include "sdkconfig.h"

namespace logcat {

namespace {

constexpr char kMountPoint[] = "/logs";
constexpr char kPartition[] = "logs";

static_assert(sizeof(kMountPoint) - 1 + 1 + kMaxFileName < CONFIG_LITTLEFS_OBJ_NAME_LEN,
              "a full path must fit LittleFS's name length");

bool valid_name(const char* name) {
    return name != nullptr && name[0] != '\0' && std::strlen(name) <= kMaxFileName &&
           std::strchr(name, '/') == nullptr;
}

}  // namespace

const char* to_string(MountResult result) {
    switch (result) {
        case MountResult::Mounted:
            return "mounted";
        case MountResult::Formatted:
            return "formatted";
        case MountResult::Failed:
            return "failed";
    }
    return "failed";
}

MountResult LittleFsFileSystem::begin() {
    if (mount_ != MountResult::Failed) {
        return mount_;
    }
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = kMountPoint;
    conf.partition_label = kPartition;
    conf.format_if_mount_failed = false;
    if (esp_vfs_littlefs_register(&conf) == ESP_OK) {
        mount_ = MountResult::Mounted;
        return mount_;
    }
    // A blank or corrupt partition: format it once, and say so (the boot line reports it).
    conf.format_if_mount_failed = true;
    mount_ =
        esp_vfs_littlefs_register(&conf) == ESP_OK ? MountResult::Formatted : MountResult::Failed;
    return mount_;
}

bool LittleFsFileSystem::usage(size_t& used, size_t& total) const {
    used = 0;
    total = 0;
    return mount_ != MountResult::Failed && esp_littlefs_info(kPartition, &total, &used) == ESP_OK;
}

bool LittleFsFileSystem::path(const char* name, char (&out)[kPathCapacity]) const {
    if (mount_ == MountResult::Failed || !valid_name(name)) {
        return false;
    }
    std::snprintf(out, sizeof(out), "%s/%s", kMountPoint, name);
    return true;
}

bool LittleFsFileSystem::is_open(const char* name) const {
    return fd_ >= 0 && name != nullptr && std::strcmp(open_name_, name) == 0;
}

bool LittleFsFileSystem::open_append(const char* name) {
    close();
    char full[kPathCapacity];
    if (!path(name, full)) {
        return false;
    }
    fd_ = ::open(full, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd_ < 0) {
        return false;
    }
    std::strncpy(open_name_, name, kMaxFileName);
    open_name_[kMaxFileName] = '\0';
    return true;
}

size_t LittleFsFileSystem::write(const char* data, size_t length) {
    if (fd_ < 0) {
        return 0;
    }
    const ssize_t written = ::write(fd_, data, length);
    return written < 0 ? 0 : static_cast<size_t>(written);  // NOSPC fails whole: 0
}

bool LittleFsFileSystem::sync() {
    return fd_ >= 0 && ::fsync(fd_) == 0;
}

void LittleFsFileSystem::close() {
    if (fd_ >= 0) {
        ::close(fd_);  // syncs
        fd_ = -1;
        open_name_[0] = '\0';
    }
}

int32_t LittleFsFileSystem::size(const char* name) {
    char full[kPathCapacity];
    struct stat info;
    if (!path(name, full) || ::stat(full, &info) != 0 || !S_ISREG(info.st_mode)) {
        return -1;
    }
    return static_cast<int32_t>(info.st_size);
}

size_t LittleFsFileSystem::read(const char* name, uint32_t offset, char* out, size_t length) {
    char full[kPathCapacity];
    if (length == 0 || !path(name, full)) {
        return 0;
    }
    const int fd = ::open(full, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    size_t got = 0;
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(offset)) {
        const ssize_t count = ::read(fd, out, length);
        got = count < 0 ? 0 : static_cast<size_t>(count);
    }
    ::close(fd);
    return got;
}

bool LittleFsFileSystem::remove(const char* name) {
    char full[kPathCapacity];
    if (is_open(name) || !path(name, full)) {
        return false;
    }
    return ::unlink(full) == 0;
}

bool LittleFsFileSystem::rename(const char* from, const char* to) {
    char source[kPathCapacity];
    char target[kPathCapacity];
    struct stat info;
    if (is_open(from) || !path(from, source) || !path(to, target)) {
        return false;
    }
    if (::stat(target, &info) == 0) {
        return false;  // LittleFS would replace it: the port never does
    }
    return ::rename(source, target) == 0;
}

size_t LittleFsFileSystem::list(NameVisitor& visitor) {
    if (mount_ == MountResult::Failed) {
        return 0;
    }
    DIR* dir = ::opendir(kMountPoint);
    if (dir == nullptr) {
        return 0;
    }
    size_t count = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_type == DT_REG) {
            visitor.visit(entry->d_name);
            ++count;
        }
    }
    ::closedir(dir);
    return count;
}

}  // namespace logcat
