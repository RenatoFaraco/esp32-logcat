#pragma once

#include <cstddef>

#include "logcat/line_formatter.hpp"
#include "logcat/log_record.hpp"

namespace logcat {

// A line already formatted by ESP-IDF's logger (log v1, LOG_FORMAT):
//   "L (timestamp) TAG: message\n", optionally wrapped in ANSI color codes.
// The tag ends at the first ':'; the space after it is optional, because the Wi-Fi blob logs
// "wifi:message" with no space.
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

// The hook's buffer for the vsnprintf output (the line itself is formatted later, by the
// writer task). 171 bytes fit "L (4294967295) ", a 15-char tag, ": " and a 127-char message;
// a longer tag only shortens the message (cut).
inline constexpr size_t kCaptureBufferSize = kLineBufferSize;

// Result of capture_idf_line: NUL-terminated strings inside the caller's buffer.
struct CapturedLine {
    LogLevel level;
    const char* tag;
    const char* message;
    bool cut;  // vsnprintf truncated the text: the message is incomplete
};

// Interprets `buffer` after `printed = vsnprintf(buffer, capacity, ...)`. Returns false when the
// text must be passed through unchanged: a vsnprintf error, anything that is not a log line, or
// complete text without a final '\n' (a line written in several calls). On success the tag and
// the message are NUL-terminated in place (over the ':' and the '\n'); nothing is written past
// buffer[capacity - 1].
bool capture_idf_line(char* buffer, size_t capacity, int printed, CapturedLine& out);

}  // namespace logcat
