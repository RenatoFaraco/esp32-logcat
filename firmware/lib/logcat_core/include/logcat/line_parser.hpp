#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/log_record.hpp"

namespace logcat {

// Why a line was not accepted. Callers count these so a dump can say "N torn, M garbage".
enum class ParseStatus : uint8_t {
    Ok,         // syntactically valid; without a checksum (ADR-007) not proof of integrity
    Torn,       // no final '\n' (cut by power loss) or closed by the store with DEL + '\n'
    Garbage,    // a byte outside 0x20-0x7E before the final '\n' (erased flash, CR, NUL, DEL)
    BadHeader,  // wrong field layout, non-digit in a number, bad tag
    BadLevel,   // level letter outside E/W/I/D/V (the formatter prints '?' for invalid levels)
    Overlong,   // more than kMaxLineLength bytes, or a message over 127 chars
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
// Preconditions: `line` points to `length` readable bytes (may be nullptr only if length is
// 0). Callers split the input at '\n' with a buffer of kLineBufferSize bytes, so a long run of
// bytes without '\n' arrives as one overlong chunk and never as a fake new line (#23, #16).
//
// Torn-line detection relies on a store invariant: nothing is appended to a file whose last
// byte is not '\n' before closing it with DEL + '\n' (#23). A torn tail with the next record
// glued to it is syntactically valid and cannot be detected here.
//
// Pure and reentrant (no state). `out` is written only when the result is ParseStatus::Ok.
ParseStatus parse_line(const char* line, size_t length, LogRecord& out);

}  // namespace logcat
