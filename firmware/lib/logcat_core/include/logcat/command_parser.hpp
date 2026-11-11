#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/log_record.hpp"

namespace logcat {

// The console commands (PRD F9, plus `info`/`stats` from #27).
enum class CommandKind : uint8_t {
    Info,
    Stats,
    LogDump,
    LogTail,
    LogClear,
    LogLevel,  // `log level <tag> <E|W|I|D|V|N>`: Command::tag and Command::level
    Reboot,
    CrashPanic,
    CrashWdt,
};

enum class CommandStatus : uint8_t {
    Ok,
    Empty,       // nothing but spaces and the line ending: no reply
    BadId,       // the first word is not an id; reply with id "-"
    BadCommand,  // unknown command (or none after the id)
    BadArgs,     // known command with missing, extra or invalid arguments; also a line with a
                 // byte outside 0x20-0x7E or longer than kMaxRequestLength (then the id is "-")
};

// Short lowercase names ("log_level", "bad_args"...) for tests and reports.
const char* to_string(CommandKind kind);
const char* to_string(CommandStatus status);

// The error code of the reply (wire-format §2.2) for a rejected request: "bad_command" or
// "bad_args". nullptr for Ok and Empty, which have no error reply.
const char* error_code(CommandStatus status);

// An id is "-" or 1 to 9999 without a leading zero: at most 4 chars.
inline constexpr size_t kRequestIdCapacity = 5;
// Longest request, line ending not counted. Longer lines are rejected whole, never cut.
inline constexpr size_t kMaxRequestLength = 64;

struct Command {
    CommandKind kind;
    // What every reply line must echo. Written for every status except Empty: the request's
    // own id, or "-" when it had none that can be echoed (BadId, a damaged or overlong line).
    char id[kRequestIdCapacity];
    // Only for LogLevel. `tag` is "*" for the default level, and is cut to the 15 chars a
    // LogRecord keeps, so it matches the stored tag (TagLevelFilter compares the same 15).
    char tag[kTagCapacity];
    LogLevel level;  // None = silence the tag
};

// Parses one console request, `<id> <command> [arguments]` (wire-format §2.1). `line` holds
// `length` bytes and is not a C string; the line ending is optional and may be "\n", "\r\n" or
// "\r" (what serial terminals send). Words are separated by one or more spaces; spaces around
// the line are ignored. Commands are lowercase and exact; level letters are uppercase.
//
// Strict where it matters: a request is either fully understood or rejected, never guessed.
// `out.kind`, `out.tag` and `out.level` are written only for Ok; `out.id` for everything but
// Empty (see Command::id).
//
// Preconditions: `line` points to `length` readable bytes (may be nullptr only if length is 0).
// Pure and reentrant: no state, no heap.
CommandStatus parse_command(const char* line, size_t length, Command& out);

}  // namespace logcat
