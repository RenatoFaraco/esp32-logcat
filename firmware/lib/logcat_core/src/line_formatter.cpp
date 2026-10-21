#include "logcat/line_formatter.hpp"

namespace logcat {

namespace {

// Writes value as exactly `width` zero-padded decimal digits. Hand-rolled instead of snprintf:
// the widths are fixed, it needs no stack-hungry printf machinery and runs in constant time.
char* write_digits(char* out, uint32_t value, size_t width) {
    for (size_t i = width; i > 0; --i) {
        out[i - 1] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return out + width;
}

constexpr bool is_printable(unsigned char byte) {
    return byte >= 0x20 && byte <= 0x7E;
}

char sanitize_message_char(char c) {
    return is_printable(static_cast<unsigned char>(c)) ? c : '?';
}

// Space and ':' would make "TAG: msg" ambiguous to split.
char sanitize_tag_char(char c) {
    if (c == ' ' || c == ':') {
        return '_';
    }
    return sanitize_message_char(c);
}

// strnlen without relying on a POSIX extension.
size_t bounded_length(const char* text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

}  // namespace

size_t format_line(const LogRecord& record, char (&out)[kLineBufferSize]) {
    char* cursor = out;

    cursor = write_digits(cursor, record.boot % kBootModulo, kBootDigits);
    *cursor++ = ':';
    cursor = write_digits(cursor, record.seq % kSeqModulo, kSeqDigits);
    *cursor++ = ' ';
    cursor = write_digits(cursor, record.uptime_ms, kUptimeDigits);
    *cursor++ = ' ';
    *cursor++ = level_to_char(record.level);
    *cursor++ = ' ';

    const size_t tag_length = bounded_length(record.tag, kTagCapacity - 1);
    if (tag_length == 0) {
        *cursor++ = '-';
    }
    for (size_t i = 0; i < tag_length; ++i) {
        *cursor++ = sanitize_tag_char(record.tag[i]);
    }
    *cursor++ = ':';
    *cursor++ = ' ';

    const size_t message_limit =
        record.truncated ? kMsgCapacity - 1 - kTruncationMarkerLength : kMsgCapacity - 1;
    const size_t message_length = bounded_length(record.msg, message_limit);
    for (size_t i = 0; i < message_length; ++i) {
        *cursor++ = sanitize_message_char(record.msg[i]);
    }
    if (record.truncated) {
        for (size_t i = 0; i < kTruncationMarkerLength; ++i) {
            *cursor++ = kTruncationMarker[i];
        }
    }

    *cursor++ = '\n';
    *cursor = '\0';
    return static_cast<size_t>(cursor - out);
}

}  // namespace logcat
