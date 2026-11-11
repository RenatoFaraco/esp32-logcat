#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/line_formatter.hpp"

namespace logcat {

// The console's reply lines (wire-format §2.2 and §3, ADR-012):
//
//   #> <id> key=value [key=value ...]    data
//   #. <id> ok                           end of the reply, success
//   #! <id> <code> [text]                end of the reply, error
//   #BEGIN <id> <name> <bytes>           dump block, followed by exactly <bytes> bytes
//   #END <id> <crc32>                    CRC-32 of those bytes, then "#." or "#!"
//
// Every function writes one whole line ending in '\n' (NUL-terminated, like format_line) and
// returns its length, or returns 0 and writes nothing useful when the line would be malformed:
// a bad id, key, value, code or name, or more than kMaxLineLength bytes. A malformed line is
// never sent: the host would show it as raw and wait for a terminator that never comes.
//
// Pure functions over the caller's buffer: no state, no heap. The console task builds the
// lines; only the writer task sends them (ADR-012 §5).

// True for "-" or 1 to 9999 without a leading zero. nullptr is not an id.
bool is_reply_id(const char* id);

// A data line, built field by field:
//
//   char line[kLineBufferSize];
//   ReplyData data(line, command.id);
//   data.add("fw", "0.3.0");
//   data.add_uint("uptime_ms", uptime);
//   send(line, data.finish());
//
// A field the firmware cannot measure is left out, never sent as 0 (ADR-012 §2).
class ReplyData {
public:
    ReplyData(char (&line)[kLineBufferSize], const char* id);

    // Appends " key=value". Returns false and leaves the line as it was when the key is not
    // [a-z][a-z0-9_.]{0,31}, the value is empty or has a byte outside 0x21-0x7E (no spaces),
    // the key is already on this line, or the field does not fit: finish this line and put
    // the field on the next one.
    bool add(const char* key, const char* value);
    bool add_uint(const char* key, uint32_t value);

    size_t fields() const { return fields_; }

    // Closes the line. 0 when the id was invalid or no field was added.
    size_t finish();

private:
    bool has_key(const char* key, size_t key_length) const;

    char* line_;
    size_t length_ = 0;  // 0 = invalid id
    size_t fields_ = 0;
};

size_t format_reply_ok(const char* id, char (&out)[kLineBufferSize]);

// `code`: [a-z_]{1,32}, never "ok" (success is only "#."). `text` is for people and optional
// (nullptr or ""): printable ASCII starting with a non-space. A text that does not fit is cut
// at the line limit: the code is what tools read, and it must arrive.
size_t format_reply_error(const char* id, const char* code, const char* text,
                          char (&out)[kLineBufferSize]);

// `name`: [A-Za-z0-9_][A-Za-z0-9._-]{0,63}, a label and not a path. `bytes` is the size of the
// content at the start of the dump.
size_t format_dump_begin(const char* id, const char* name, uint32_t bytes,
                         char (&out)[kLineBufferSize]);

// `crc`: crc32_update() over exactly the bytes announced by format_dump_begin.
size_t format_dump_end(const char* id, uint32_t crc, char (&out)[kLineBufferSize]);

// CRC-32 IEEE (reflected polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF): the
// one of zlib.crc32, with the same chaining. Start with 0 and feed the content in pieces:
//
//   uint32_t crc = 0;
//   crc = crc32_update(crc, part1, n1);
//   crc = crc32_update(crc, part2, n2);
//
// 16-entry table: 64 bytes of flash and two lookups per byte, instead of the 1 KB table.
uint32_t crc32_update(uint32_t crc, const void* data, size_t length);

}  // namespace logcat
