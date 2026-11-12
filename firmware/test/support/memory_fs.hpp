// A FileSystem in memory, for the native tests of the store (#22, #23) and the power-cut tests
// (M6). Test-only code: std::string and std::map are fine here (never in the core).
//
// It follows the strict side of the port's contract (logcat/file_system.hpp), so what passes
// here also holds on LittleFS:
// - size() and read() of the open file see only what was synced;
// - after a short write the open file is in error: sync() fails and close() keeps only what
//   was synced before;
// - remove() and rename() refuse the open file; rename() refuses an existing target.
// And it models what matters for persistence:
// - power_cut(): every file goes back to its last sync. `extra` keeps some of the bytes
//   written after it, which LittleFS never does (it rolls back atomically); it stands for
//   other file systems and for defensive tests;
// - a full partition, optionally with LittleFS-like blocks: each file uses whole blocks,
//   plus a few blocks of metadata;
// - failures on demand: the next open, write, sync, rename or remove fails once.
#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "logcat/file_system.hpp"

namespace test_support {

class MemoryFileSystem final : public logcat::FileSystem {
public:
    // `block_size` 1 counts bytes exactly; 4096 with a few `overhead_blocks` is closer to
    // LittleFS on the 512 KB partition.
    explicit MemoryFileSystem(size_t capacity = 512 * 1024, size_t block_size = 1,
                              size_t overhead_blocks = 0)
        : capacity_(capacity), block_size_(block_size), overhead_blocks_(overhead_blocks) {}

    // ---------------------------------------------------------------- FileSystem

    bool open_append(const char* name) override {
        close();
        ++opens;
        if (fail_next_open) {
            fail_next_open = false;
            return false;
        }
        if (!valid_name(name)) {
            return false;
        }
        File& file = files_[name];  // created empty if it does not exist
        file.erred = false;
        open_ = name;
        return true;
    }

    size_t write(const char* data, size_t length) override {
        ++writes;
        const bool fail = fail_next_write;
        fail_next_write = false;
        if (open_.empty()) {
            return 0;
        }
        File& file = files_[open_];
        const size_t room = room_for_open();
        size_t accepted = std::min(length, room > file.data.size() ? room - file.data.size() : 0);
        if (fail) {
            accepted = std::min(accepted, short_write);
        }
        file.data.append(data, accepted);
        bytes_written += accepted;
        if (accepted < length) {
            file.erred = true;
        }
        return accepted;
    }

    bool sync() override {
        ++syncs;
        return commit();
    }

    void close() override {
        if (open_.empty()) {
            return;
        }
        ++closes;
        commit();
        File& file = files_[open_];
        if (file.erred) {
            file.data.resize(file.synced);  // what the handle took since its last sync is gone
            file.erred = false;
        }
        open_.clear();
    }

    int32_t size(const char* name) override {
        const File* file = find(name);
        return file == nullptr ? -1 : static_cast<int32_t>(visible(name, *file));
    }

    size_t read(const char* name, uint32_t offset, char* out, size_t length) override {
        const File* file = find(name);
        if (file == nullptr) {
            return 0;
        }
        const size_t end = visible(name, *file);
        if (offset >= end) {
            return 0;
        }
        const size_t count = std::min(length, end - offset);
        std::memcpy(out, file->data.data() + offset, count);
        return count;
    }

    bool remove(const char* name) override {
        if (fail_next_remove) {
            fail_next_remove = false;
            return false;
        }
        if (!valid_name(name) || open_ == name) {
            return false;
        }
        return files_.erase(name) == 1;
    }

    bool rename(const char* from, const char* to) override {
        if (fail_next_rename) {
            fail_next_rename = false;
            return false;
        }
        if (!valid_name(from) || !valid_name(to) || open_ == from) {
            return false;
        }
        const auto found = files_.find(from);
        if (found == files_.end() || files_.count(to) != 0) {
            return false;
        }
        File file = found->second;
        files_.erase(found);
        files_[to] = file;
        return true;
    }

    size_t list(logcat::NameVisitor& visitor) override {
        // A copy first: a visitor that breaks the rule and changes the files must not leave an
        // invalid iterator behind.
        std::vector<std::string> names;
        for (const auto& entry : files_) {
            names.push_back(entry.first);
        }
        for (const std::string& name : names) {
            visitor.visit(name.c_str());
        }
        return names.size();
    }

    // ---------------------------------------------------------------- test controls

    // Power is lost: every file keeps what was synced, plus up to `extra` of the bytes written
    // after the last sync (not LittleFS's behaviour: see the top of this file); nothing is open.
    void power_cut(size_t extra = 0) {
        for (auto& entry : files_) {
            File& file = entry.second;
            const size_t keep = std::min(file.data.size(), file.synced + extra);
            file.data.resize(keep);
            file.synced = keep;
            file.erred = false;
        }
        open_.clear();
    }

    // Everything a file holds, synced or not ("" if it does not exist), and a way to shape one.
    std::string content(const std::string& name) const {
        const auto found = files_.find(name);
        return found == files_.end() ? std::string() : found->second.data;
    }
    void put(const std::string& name, const std::string& data) {
        files_[name] = File{data, data.size(), false};
    }
    bool exists(const std::string& name) const { return files_.count(name) != 0; }
    size_t file_count() const { return files_.size(); }
    const std::string& open_file() const { return open_; }
    // Space in use: bytes, or whole blocks per file plus the metadata blocks.
    size_t used() const { return used_with(nullptr, 0); }
    void set_capacity(size_t capacity) { capacity_ = capacity; }

    bool fail_next_open = false;
    bool fail_next_write = false;
    size_t short_write = 0;  // bytes the failing write still accepts
    bool fail_next_sync = false;
    bool fail_next_rename = false;
    bool fail_next_remove = false;

    size_t opens = 0;
    size_t writes = 0;
    size_t syncs = 0;   // explicit sync() calls only
    size_t closes = 0;  // closes of an open file (each one syncs too)
    size_t bytes_written = 0;

private:
    struct File {
        std::string data;    // everything written, synced or not
        size_t synced = 0;   // bytes that survive a power cut and that size()/read() show
        bool erred = false;  // a short write happened since the last sync
    };

    static bool valid_name(const char* name) {
        if (name == nullptr || name[0] == '\0') {
            return false;
        }
        return std::strlen(name) <= logcat::kMaxFileName && std::strchr(name, '/') == nullptr;
    }

    const File* find(const char* name) const {
        if (!valid_name(name)) {
            return nullptr;
        }
        const auto found = files_.find(name);
        return found == files_.end() ? nullptr : &found->second;
    }

    size_t visible(const char* name, const File& file) const {
        return open_ == name ? file.synced : file.data.size();
    }

    bool commit() {
        const bool fail = fail_next_sync;
        fail_next_sync = false;
        if (open_.empty()) {
            return false;
        }
        File& file = files_[open_];
        if (fail || file.erred) {
            return false;
        }
        file.synced = file.data.size();
        return true;
    }

    size_t blocks(size_t bytes) const { return (bytes + block_size_ - 1) / block_size_; }

    // Space in use if `name` (when given) held `bytes` instead.
    size_t used_with(const std::string* name, size_t bytes) const {
        size_t total = overhead_blocks_ * block_size_;
        for (const auto& entry : files_) {
            const size_t size =
                name != nullptr && entry.first == *name ? bytes : entry.second.data.size();
            total += blocks(size) * block_size_;
        }
        return total;
    }

    // The most bytes the open file can hold: whole blocks left after the others.
    size_t room_for_open() const {
        const size_t others = used_with(&open_, 0);
        return others >= capacity_ ? 0 : (capacity_ - others) / block_size_ * block_size_;
    }

    size_t capacity_;
    size_t block_size_;
    size_t overhead_blocks_;
    std::map<std::string, File> files_;
    std::string open_;
};

}  // namespace test_support
