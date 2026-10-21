#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/log_record.hpp"

namespace logcat {

// Why a line was not accepted. Callers count these so a dump can say "N torn, M garbage".
enum class ParseStatus : uint8_t {
    Ok,
    Torn,       // no final '\n' (cut by power loss) or closed by the store with DEL + '\n'
    Garbage,    // a byte outside 0x20-0x7E before the final '\n' (erased flash, CR, NUL...)
    BadHeader,  // wrong field layout, non-digit in a number, bad tag, message over 127 chars
    BadLevel,   // level letter outside E/W/I/D/V (the formatter prints '?' for invalid levels)
    Overlong,   // more than kMaxLineLength bytes
};

// Short lowercase name ("torn", "bad_header"...) for counters and reports.
const char* to_string(ParseStatus status);

// Parses one line of the PRD §3 format. `line` holds `length` bytes *including* the final
// '\n'; it is not a C string (data read back from flash may contain '\0').
//
// Strict by design: when in doubt, reject. A corrupted line taken as data is worse than a lost
// line that is counted. No ordering of boot/seq/uptime is assumed. The message is kept raw
// (edge spaces and a final "..." included), so format_line(parsed) reproduces the line.
//
// `out` is written only when the result is ParseStatus::Ok.
ParseStatus parse_line(const char* line, size_t length, LogRecord& out);

}  // namespace logcat
