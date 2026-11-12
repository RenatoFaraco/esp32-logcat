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

RotatingStore::RotatingStore(FileSystem& fs, StoreConfig config) : fs_(fs), config_(config) {
    // One file can never be larger than everything together.
    if (config_.file_limit > config_.total_limit) {
        config_.file_limit = config_.total_limit;
    }
}

bool RotatingStore::begin() {
    fs_.close();
    open_ = false;
    pending_ = 0;
    scan();
    // Limits may have shrunk since these files were written: the oldest go first.
    while (count_ > 1 && total() > config_.total_limit) {
        if (!remove_oldest()) {
            break;
        }
    }
    if (count_ == 0) {
        files_[0] = Entry{0, 0, false};
        count_ = 1;
    }
    return open_current();
}

void RotatingStore::scan() {
    IndexScan scan;
    fs_.list(scan);
    count_ = 0;
    // Newest kMaxStoreFiles, oldest first. More can only exist if the limits shrank, or a file
    // could not be removed: the extra ones are removed now (one that will not go is an orphan:
    // renumber() clears its name if it ever needs it).
    uint32_t found = 0;
    for (uint32_t index = kIndexSlots; index-- > 0;) {
        if (!scan.has(index)) {
            continue;
        }
        char name[kStoreNameCapacity];
        store_file_name(index, name);
        if (found < kMaxStoreFiles) {
            const int32_t size = fs_.size(name);
            // A size that cannot be read is charged as a full file: the limit stays safe.
            files_[kMaxStoreFiles - 1 - found] =
                size < 0 ? Entry{index, config_.file_limit, true}
                         : Entry{index, static_cast<uint32_t>(size), false};
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
    pending_ = 0;
    const int32_t size = fs_.size(name);
    if (size >= 0) {
        current().size = static_cast<uint32_t>(size);
        current().size_unknown = false;
        char last = '\n';
        if (current().size > 0 && fs_.read(name, current().size - 1, &last, 1) != 1) {
            last = '\0';  // unreadable: repair rather than risk gluing a line to it
        }
        needs_repair_ = last != '\n';
    } else {
        // Missing (a new file: nothing to repair) or unreadable (a file the scan found: it may
        // end torn, so it is repaired; its size stays charged as full).
        needs_repair_ = current().size_unknown;
        if (!current().size_unknown) {
            current().size = 0;
        }
    }
    open_ = fs_.open_append(name);
    return open_;
}

bool RotatingStore::reopen() {
    if (count_ == 0) {
        return false;  // begin() was never called
    }
    if (open_current()) {
        return true;
    }
    // A newest file that holds data and will not open would stop all logging: move on to the
    // next index. Only once per such file: an empty one that fails too is not skipped, so a
    // broken file system cannot churn through the indices and remove the history.
    const bool had_data = current().size > 0 || current().size_unknown;
    return had_data && rotate();
}

bool RotatingStore::write(const char* line, size_t length) {
    if (length == 0) {
        return true;
    }
    if (!open_ && !reopen()) {
        ++stats_.failed_writes;
        return false;
    }
    // A torn tail is closed first, in this file, even when the line then goes to a new one:
    // the piece must never stay open-ended at the end of a file (a dump joining the files
    // would glue it to the next one). The file can so end up to 2 bytes over file_limit.
    if (needs_repair_ && !repair()) {
        ++stats_.failed_writes;
        return false;
    }
    if (current().size > 0 && current().size + length > config_.file_limit && !rotate()) {
        ++stats_.failed_writes;
        return false;
    }
    if (!make_room(length)) {
        ++stats_.failed_writes;
        return false;
    }
    const size_t written = fs_.write(line, length);
    current().size += static_cast<uint32_t>(written);
    pending_ += static_cast<uint32_t>(written);
    if (written != length) {
        lose_pending();
        ++stats_.failed_writes;
        return false;
    }
    return true;
}

bool RotatingStore::repair() {
    if (!make_room(kTornCloseLength)) {
        return false;
    }
    const size_t written = fs_.write(kTornClose, kTornCloseLength);
    current().size += static_cast<uint32_t>(written);
    pending_ += static_cast<uint32_t>(written);
    if (written != kTornCloseLength) {
        lose_pending();  // reopened (and repaired again) before the next line
        return false;
    }
    needs_repair_ = false;
    ++stats_.repaired;
    return true;
}

// The file is in error (port contract): what it took since the last sync is gone. Closed now,
// reopened (re-reading its real size, repairing its tail) before the next line.
void RotatingStore::lose_pending() {
    stats_.lost_bytes += pending_;
    pending_ = 0;
    fs_.close();
    open_ = false;
}

bool RotatingStore::flush() {
    if (!open_) {
        return true;  // nothing open, nothing pending
    }
    if (fs_.sync()) {
        pending_ = 0;
        return true;
    }
    ++stats_.sync_failures;
    lose_pending();
    return false;
}

bool RotatingStore::rotate() {
    fs_.close();  // syncs: nothing is pending in the old file any more
    open_ = false;
    pending_ = 0;
    if (current().index >= kMaxFileIndex && !renumber()) {
        return false;
    }
    if (count_ == kMaxStoreFiles && !remove_oldest()) {
        return false;
    }
    files_[count_] = Entry{current().index + 1, 0, false};
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
        // A file there is an orphan (one the scan could not remove): never a tracked one, which
        // all sit at or above i. It must not block the rename.
        fs_.remove(to);
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
