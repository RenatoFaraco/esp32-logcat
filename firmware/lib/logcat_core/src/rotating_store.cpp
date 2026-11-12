#include "logcat/rotating_store.hpp"

#include <cstring>

namespace logcat {

namespace {

constexpr char kTornClose[] = "\x7F\n";  // DEL + '\n': the parser's torn sentinel
constexpr size_t kTornCloseLength = 2;
constexpr uint32_t kIndexSlots = kMaxFileIndex + 1;

// Index of "log_NNN.txt", or kIndexSlots for any other name.
uint32_t index_of(const char* name) {
    if (std::strlen(name) != kStoreNameCapacity - 1 || std::strncmp(name, "log_", 4) != 0 ||
        std::strcmp(name + 7, ".txt") != 0) {
        return kIndexSlots;
    }
    uint32_t index = 0;
    for (size_t i = 4; i < 7; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return kIndexSlots;
        }
        index = index * 10 + static_cast<uint32_t>(name[i] - '0');
    }
    return index;
}

// Collects which indices exist, without heap: one bit per possible index.
class IndexScan final : public NameVisitor {
public:
    void visit(const char* name) override {
        const uint32_t index = index_of(name);
        if (index < kIndexSlots) {
            seen_[index / 8] = static_cast<uint8_t>(seen_[index / 8] | (1u << (index % 8)));
        }
    }
    bool has(uint32_t index) const { return (seen_[index / 8] >> (index % 8)) & 1u; }

private:
    uint8_t seen_[(kIndexSlots + 7) / 8] = {};
};

}  // namespace

void store_file_name(uint32_t index, char (&out)[kStoreNameCapacity]) {
    std::memcpy(out, "log_000.txt", kStoreNameCapacity);
    out[4] = static_cast<char>('0' + index / 100 % 10);
    out[5] = static_cast<char>('0' + index / 10 % 10);
    out[6] = static_cast<char>('0' + index % 10);
}

RotatingStore::RotatingStore(FileSystem& fs, StoreConfig config) : fs_(fs), config_(config) {}

bool RotatingStore::begin() {
    fs_.close();
    open_ = false;
    scan();
    // Limits may have shrunk since these files were written: the oldest go first.
    while (count_ > 1 && total() > config_.total_limit) {
        if (!remove_oldest()) {
            break;
        }
    }
    if (count_ == 0) {
        files_[0] = Entry{0, 0};
        count_ = 1;
    }
    return open_current();
}

void RotatingStore::scan() {
    IndexScan scan;
    fs_.list(scan);
    count_ = 0;
    // Newest kMaxStoreFiles, oldest first. More can only exist if the limits shrank, or a file
    // could not be removed: the extra ones are removed now.
    uint32_t found = 0;
    for (uint32_t index = kIndexSlots; index-- > 0;) {
        if (!scan.has(index)) {
            continue;
        }
        char name[kStoreNameCapacity];
        store_file_name(index, name);
        if (found < kMaxStoreFiles) {
            const int32_t size = fs_.size(name);
            files_[kMaxStoreFiles - 1 - found] =
                Entry{index, size < 0 ? 0u : static_cast<uint32_t>(size)};
            ++found;
        } else if (fs_.remove(name)) {
            ++stats_.removed;
        } else {
            ++stats_.failed_removes;
        }
    }
    // Move the found entries to the front, keeping their order.
    for (uint32_t i = 0; i < found; ++i) {
        files_[i] = files_[kMaxStoreFiles - found + i];
    }
    count_ = found;
}

bool RotatingStore::open_current() {
    char name[kStoreNameCapacity];
    store_file_name(current().index, name);
    const int32_t size = fs_.size(name);
    current().size = size < 0 ? 0u : static_cast<uint32_t>(size);
    char last = '\n';
    if (current().size > 0 && fs_.read(name, current().size - 1, &last, 1) != 1) {
        last = '\0';  // unreadable: repair rather than risk gluing a line to it
    }
    needs_repair_ = last != '\n';
    open_ = fs_.open_append(name);
    return open_;
}

bool RotatingStore::write(const char* line, size_t length) {
    if (length == 0) {
        return true;
    }
    if (!open_ && (count_ == 0 || !open_current())) {
        ++stats_.failed_writes;
        return false;
    }
    if (current().size > 0 && current().size + length > config_.file_limit && !rotate()) {
        ++stats_.failed_writes;
        return false;
    }
    if (!make_room(length + (needs_repair_ ? kTornCloseLength : 0))) {
        ++stats_.failed_writes;
        return false;
    }
    if (needs_repair_) {
        const size_t written = fs_.write(kTornClose, kTornCloseLength);
        current().size += static_cast<uint32_t>(written);
        if (written != kTornCloseLength) {
            fs_.close();  // in error: reopened (and repaired again) before the next line
            open_ = false;
            ++stats_.failed_writes;
            return false;
        }
        needs_repair_ = false;
        ++stats_.repaired;
    }
    const size_t written = fs_.write(line, length);
    current().size += static_cast<uint32_t>(written);
    if (written != length) {
        fs_.close();
        open_ = false;
        ++stats_.failed_writes;
        return false;
    }
    return true;
}

bool RotatingStore::flush() {
    return open_ && fs_.sync();
}

bool RotatingStore::rotate() {
    fs_.close();
    open_ = false;
    if (current().index >= kMaxFileIndex && !renumber()) {
        return false;
    }
    if (count_ == kMaxStoreFiles && !remove_oldest()) {
        return false;
    }
    files_[count_] = Entry{current().index + 1, 0};
    ++count_;
    ++stats_.rotations;
    return open_current();
}

// After log_999: the files are renamed to log_000, log_001... in the same order. Ascending, so
// a target name is never one still to be renamed.
bool RotatingStore::renumber() {
    for (size_t i = 0; i < count_; ++i) {
        if (files_[i].index == i) {
            continue;
        }
        char from[kStoreNameCapacity];
        char to[kStoreNameCapacity];
        store_file_name(files_[i].index, from);
        store_file_name(static_cast<uint32_t>(i), to);
        if (!fs_.rename(from, to)) {
            return false;
        }
        files_[i].index = static_cast<uint32_t>(i);
    }
    ++stats_.renumbered;
    return true;
}

// Removes the oldest files (never the current one) until `length` more bytes fit the total.
bool RotatingStore::make_room(size_t length) {
    while (total() + length > config_.total_limit) {
        if (count_ <= 1 || !remove_oldest()) {
            return false;  // refused: the partition must never pass the limit
        }
    }
    return true;
}

bool RotatingStore::remove_oldest() {
    char name[kStoreNameCapacity];
    store_file_name(files_[0].index, name);
    if (!fs_.remove(name)) {
        ++stats_.failed_removes;
        return false;
    }
    forget_oldest();
    ++stats_.removed;
    return true;
}

void RotatingStore::forget_oldest() {
    for (size_t i = 1; i < count_; ++i) {
        files_[i - 1] = files_[i];
    }
    --count_;
}

uint32_t RotatingStore::total() const {
    uint32_t sum = 0;
    for (size_t i = 0; i < count_; ++i) {
        sum += files_[i].size;
    }
    return sum;
}

StoreStats RotatingStore::stats() const {
    StoreStats out = stats_;
    out.files = static_cast<uint32_t>(count_);
    out.bytes = total();
    return out;
}

bool RotatingStore::file_name(size_t position, char (&out)[kStoreNameCapacity]) const {
    if (position >= count_) {
        return false;
    }
    store_file_name(files_[position].index, out);
    return true;
}

}  // namespace logcat
