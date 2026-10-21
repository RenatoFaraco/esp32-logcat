#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/log_record.hpp"
#include "logcat/sequencer.hpp"

namespace logcat {

// Line format (PRD §3, ADR-006), one record per line, printable ASCII only:
//
//   BBBB:SSSSSS UUUUUUUUUU L TAG: MSG\n
//   0042:000137 0000123456 W PRESSURE: reading out of range (4.71 V)
//
// boot is printed modulo 10000 and seq modulo 1e6 so the header keeps a fixed width.
inline constexpr uint32_t kBootModulo = 10'000;
inline constexpr size_t kBootDigits = 4;
inline constexpr size_t kSeqDigits = 6;
inline constexpr size_t kUptimeDigits = 10;  // uint32_t max is 4294967295
static_assert(kSeqModulo == 1'000'000, "seq field width must match kSeqModulo");

// A truncated message shows at most 124 chars followed by this marker (never over 127).
inline constexpr char kTruncationMarker[] = "...";
inline constexpr size_t kTruncationMarkerLength = sizeof(kTruncationMarker) - 1;

// Longest possible line, '\n' included: header, 15-char tag, ": ", 127-char message.
inline constexpr size_t kMaxLineLength = kBootDigits + 1 + kSeqDigits + 1 + kUptimeDigits + 1 + 1 +
                                         1 + (kTagCapacity - 1) + 2 + (kMsgCapacity - 1) + 1;
static_assert(kMaxLineLength == 170, "line format changed: update both parsers and PRD §3");
inline constexpr size_t kLineBufferSize = kMaxLineLength + 1;  // + '\0'

// Formats record as a single printable-ASCII line ending in '\n' and NUL-terminates out.
// Returns the line length without the '\0', so the writer can emit it in one write.
//
// Text rules: bytes outside 0x20-0x7E become '?'; in the tag, space and ':' become '_' and an
// empty tag prints '-'. tag/msg are read only up to their capacity, so a record without
// terminators (e.g. read back from RTC memory) cannot make this read past the struct.
// Meant for the writer task: the per-byte sanitizing is never paid by the caller.
size_t format_line(const LogRecord& record, char (&out)[kLineBufferSize]);

}  // namespace logcat
