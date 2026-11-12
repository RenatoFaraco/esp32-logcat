// A FileSystem in memory, for the native tests of the store (#22, #23) and the power-cut tests
// (M6). Test-only code: std::string and std::map are fine here (never in the core).
//
// Besides the files it models what matters for persistence:
// - durability: each file remembers how much of it was synced; power_cut() throws away what
//   came after, keeping up to `extra` of those bytes, so a file can end torn anywhere;
// - a full partition: writes beyond `capacity` are short;
// - failures on demand: the next open, write, rename or remove fails.
#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "logcat/file_system.hpp"

namespace test_support {

class MemoryFileSystem final : public logcat::FileSystem {
public:
    explicit MemoryFileSystem(size_t capacity = 512 * 1024) : capacity_(capacity) {}

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
        files_[name];  // created empty if it does not exist
        open_ = name;
        return true;
    }

    size_t write(const char* data, size_t length) override {
        ++writes;
        if (open_.empty()) {
            return 0;
        }
        size_t accepted = std::min(length, capacity_ - std::min(capacity_, used()));
        if (fail_next_write) {
            fail_next_write = false;
            accepted = std::min(accepted, short_write);
        }
        files_[open_].data.append(data, accepted);
        bytes_written += accepted;
        return accepted;
    }

    bool sync() override {
        ++syncs;
        if (open_.empty()) {
            return false;
        }
        File& file = files_[open_];
        file.synced = file.data.size();
        return true;
    }

    void close() override {
        if (!open_.empty()) {
            sync();
            open_.clear();
        }
    }

    int32_t size(const char* name) override {
        const auto found = files_.find(name);
        return found == files_.end() ? -1 : static_cast<int32_t>(found->second.data.size());
    }

    size_t read(const char* name, uint32_t offset, char* out, size_t length) override {
        const auto found = files_.find(name);
        if (found == files_.end() || offset >= found->second.data.size()) {
            return 0;
        }
        const size_t count = std::min(length, found->second.data.size() - offset);
        std::memcpy(out, found->second.data.data() + offset, count);
        return count;
    }

    bool remove(const char* name) override {
        if (fail_next_remove) {
            fail_next_remove = false;
            return false;
        }
        if (open_ == name) {
            open_.clear();
        }
        return files_.erase(name) == 1;
    }

    bool rename(const char* from, const char* to) override {
        if (fail_next_rename) {
            fail_next_rename = false;
            return false;
        }
        const auto found = files_.find(from);
        if (found == files_.end() || files_.count(to) != 0 || !valid_name(to)) {
            return false;
        }
        File file = found->second;
        files_.erase(found);
        files_[to] = file;
        if (open_ == from) {
            open_ = to;
        }
        return true;
    }

    size_t list(logcat::NameVisitor& visitor) override {
        for (const auto& entry : files_) {
            visitor.visit(entry.first.c_str());
        }
        return files_.size();
    }

    // ---------------------------------------------------------------- test controls

    // Power is lost: every file keeps what was synced plus up to `extra` of the bytes written
    // after the last sync (what the flash may have taken before the cut); nothing is open.
    void power_cut(size_t extra = 0) {
        for (auto& entry : files_) {
            File& file = entry.second;
            const size_t keep = std::min(file.data.size(), file.synced + extra);
            file.data.resize(keep);
            file.synced = keep;
        }
        open_.clear();
    }

    // Content of a file ("" if it does not exist), and direct access to shape one in a test.
    std::string content(const std::string& name) const {
        const auto found = files_.find(name);
        return found == files_.end() ? std::string() : found->second.data;
    }
    void put(const std::string& name, const std::string& data) {
        files_[name] = File{data, data.size()};
    }
    bool exists(const std::string& name) const { return files_.count(name) != 0; }
    size_t file_count() const { return files_.size(); }
    const std::string& open_file() const { return open_; }
    size_t used() const {
        size_t total = 0;
        for (const auto& entry : files_) {
            total += entry.second.data.size();
        }
        return total;
    }
    void set_capacity(size_t capacity) { capacity_ = capacity; }

    bool fail_next_open = false;
    bool fail_next_write = false;
    size_t short_write = 0;  // bytes the failing write still accepts
    bool fail_next_rename = false;
    bool fail_next_remove = false;

    size_t opens = 0;
    size_t writes = 0;
    size_t syncs = 0;
    size_t bytes_written = 0;

private:
    struct File {
        std::string data;
        size_t synced = 0;  // bytes that survive a power cut
    };

    static bool valid_name(const char* name) {
        if (name == nullptr || name[0] == '\0') {
            return false;
        }
        const size_t length = std::strlen(name);
        return length <= logcat::kMaxFileName && std::strchr(name, '/') == nullptr;
    }

    size_t capacity_;
    std::map<std::string, File> files_;
    std::string open_;
};

}  // namespace test_support
