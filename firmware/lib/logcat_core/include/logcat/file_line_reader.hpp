#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/file_system.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"

namespace logcat {

// Reads a log file from flash line by line and parses each one (#23): for the dump (#29) and
// for counting what a file holds ("N torn, M garbage").
//
// Fixed buffers, no heap. Lines are cut at '\n' with a buffer of kLineBufferSize bytes, so a
// run of bytes without '\n' (erased flash, a torn tail glued to garbage) comes out as one
// overlong chunk and the rest of it, up to the next '\n', is skipped: it never becomes a fake
// new line. A last piece without '\n' is torn. The file is read in blocks of kReadBlock bytes
// (each FileSystem::read by name may open and seek the file).
//
// The size is taken when the reader is made: a file that grows meanwhile is read up to that
// point (sync it first if it is the open one, see FileSystem).
class FileLineReader {
public:
    static constexpr size_t kReadBlock = 512;

    FileLineReader(FileSystem& fs, const char* name);

    // The next line, parsed. false at the end of the file. `out` is written only when the
    // status is Ok (parse_line's rule); line() and length() hold the raw bytes either way.
    bool next(ParseStatus& status, LogRecord& out);

    const char* line() const { return line_; }
    size_t length() const { return length_; }

    // How many lines got each status so far.
    uint32_t count(ParseStatus status) const { return counts_[static_cast<size_t>(status)]; }
    uint32_t file_size() const { return size_; }

private:
    bool fill();

    FileSystem& fs_;
    char name_[kMaxFileName + 1] = {};
    uint32_t size_ = 0;
    uint32_t offset_ = 0;  // of the next block to read
    char block_[kReadBlock] = {};
    size_t block_length_ = 0;
    size_t block_position_ = 0;
    char line_[kLineBufferSize] = {};
    size_t length_ = 0;
    bool skipping_ = false;  // inside the rest of an overlong line
    uint32_t counts_[6] = {};
};

}  // namespace logcat
