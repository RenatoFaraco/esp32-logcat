#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace logcat {

// Numbering follows ESP-IDF's esp_log_level_t (ERROR = 1 ... VERBOSE = 5) for familiarity.
// The esp_log_set_vprintf hook only sees the formatted text, so the adapter gets the level
// from its letter via level_from_char, never by casting an unchecked number.
// Lower value = more severe: "enabled" means level <= threshold.
enum class LogLevel : uint8_t {
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Verbose = 5,
};

// Letter used in the line format (PRD §3). Out-of-range values map to '?'.
constexpr char level_to_char(LogLevel level) {
    switch (level) {
        case LogLevel::Error:
            return 'E';
        case LogLevel::Warn:
            return 'W';
        case LogLevel::Info:
            return 'I';
        case LogLevel::Debug:
            return 'D';
        case LogLevel::Verbose:
            return 'V';
    }
    return '?';
}

// Inverse of level_to_char. Leaves `level` untouched and returns false for unknown letters.
constexpr bool level_from_char(char letter, LogLevel& level) {
    switch (letter) {
        case 'E':
            level = LogLevel::Error;
            return true;
        case 'W':
            level = LogLevel::Warn;
            return true;
        case 'I':
            level = LogLevel::Info;
            return true;
        case 'D':
            level = LogLevel::Debug;
            return true;
        case 'V':
            level = LogLevel::Verbose;
            return true;
        default:
            return false;
    }
}

// Buffer sizes including the terminating '\0': tags up to 15 chars, messages up to 127.
inline constexpr size_t kTagCapacity = 16;
inline constexpr size_t kMsgCapacity = 128;

// One log line on its way from the caller to the writer task.
// Copied by value through the FreeRTOS queue (memcpy), so it must stay trivially copyable:
// no constructors, destructors or owning members. `LogRecord record{}` zero-initializes it.
//
// Bytes after each '\0' and the struct padding are unspecified (zeroing them would cost every
// caller a memset). Never memcmp or hash the raw struct; compare fields or the formatted line.
//
// tag and msg are stored as given. The formatter makes the line printable ASCII and single-line
// (ADR-006), so control characters or newlines in a message cannot forge extra lines.
struct LogRecord {
    uint32_t boot;       // full NVS boot counter; the line format prints 4 digits (see #5)
    uint32_t seq;        // < kSeqModulo, see Sequencer
    uint32_t uptime_ms;  // wraps after 2^32 ms (~49.7 days): not a global ordering key
    LogLevel level;
    bool truncated;  // msg was cut to fit kMsgCapacity
    char tag[kTagCapacity];
    char msg[kMsgCapacity];

    // Copies at most 15 chars; longer tags are cut silently (tags are <= 15 by convention).
    // nullptr becomes an empty tag.
    void set_tag(const char* text);

    // Copies at most 127 chars and sets `truncated` when the text did not fit.
    // nullptr becomes an empty message.
    void set_message(const char* text);
};

static_assert(std::is_trivially_copyable_v<LogRecord>, "LogRecord is copied with memcpy");
static_assert(sizeof(LogRecord) <= 160, "LogRecord exceeds the queue budget (ARCHITECTURE §8)");

}  // namespace logcat
