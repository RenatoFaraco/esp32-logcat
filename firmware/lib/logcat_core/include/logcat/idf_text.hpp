#pragma once

#include <cstddef>

#include "logcat/log_record.hpp"

namespace logcat {

// A line already formatted by ESP-IDF's logger (log v1, LOG_FORMAT):
//   "L (timestamp) TAG: message\n", optionally wrapped in ANSI color codes.
// Spans point into the caller's buffer: nothing is copied and nothing is NUL-terminated.
struct IdfText {
    LogLevel level;
    const char* tag;
    size_t tag_length;
    const char* message;
    size_t message_length;
};

// Recognizes one ESP-IDF log line in `text` (`length` bytes, not necessarily NUL-terminated).
// Accepts a missing final '\n' (text cut by a fixed buffer). Returns false for anything that is
// not a log line, so the caller can pass it through unchanged. Reads only within `length`.
// Pure text parsing: the IDF format lives here so it can be tested on the PC.
bool parse_idf_text(const char* text, size_t length, IdfText& out);

}  // namespace logcat
