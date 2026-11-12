#include "logcat/file_line_reader.hpp"

#include <cstring>

namespace logcat {

static_assert(static_cast<size_t>(ParseStatus::Overlong) == 5, "counts_ has one slot per status");

FileLineReader::FileLineReader(FileSystem& fs, const char* name) : fs_(fs) {
    if (name != nullptr) {
        std::strncpy(name_, name, kMaxFileName);
        const int32_t size = fs_.size(name_);
        size_ = size < 0 ? 0u : static_cast<uint32_t>(size);
    }
}

bool FileLineReader::fill() {
    if (offset_ >= size_) {
        return false;
    }
    size_t want = kReadBlock;
    if (size_ - offset_ < want) {
        want = size_ - offset_;
    }
    block_length_ = fs_.read(name_, offset_, block_, want);
    block_position_ = 0;
    offset_ += static_cast<uint32_t>(block_length_);
    if (block_length_ == 0) {
        offset_ = size_;  // a read error ends the file here
        return false;
    }
    return true;
}

bool FileLineReader::next(ParseStatus& status, LogRecord& out) {
    length_ = 0;
    for (;;) {
        if (block_position_ == block_length_ && !fill()) {
            if (length_ == 0) {
                return false;  // the end, with nothing pending
            }
            break;  // a last piece without '\n': the parser calls it torn
        }
        const char byte = block_[block_position_++];
        if (skipping_) {
            skipping_ = byte != '\n';
            continue;
        }
        line_[length_++] = byte;
        if (byte == '\n') {
            break;
        }
        if (length_ == sizeof(line_)) {
            // kLineBufferSize bytes without '\n': overlong (or a torn line closed with DEL that
            // just fits, which the parser recognizes). Whatever is left of it is skipped.
            skipping_ = true;
            break;
        }
    }
    // Up to 171 bytes: a torn tail closed with DEL + '\n' fits whole, and the parser tells it
    // apart from an overlong line.
    status = parse_line(line_, length_, out);
    ++counts_[static_cast<size_t>(status)];
    return true;
}

}  // namespace logcat
