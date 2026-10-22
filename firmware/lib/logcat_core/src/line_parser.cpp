#include "logcat/line_parser.hpp"

#include <cstring>

#include "logcat/line_formatter.hpp"

namespace logcat {

namespace {

constexpr char kTornSentinel = '\x7F';  // DEL, appended by the store to close a torn tail

// Header "BBBB:SSSSSS UUUUUUUUUU L " — positions of separators, level and fields.
constexpr size_t kBootStart = 0;
constexpr size_t kSeqStart = kBootStart + kBootDigits + 1;
constexpr size_t kUptimeStart = kSeqStart + kSeqDigits + 1;
constexpr size_t kLevelIndex = kUptimeStart + kUptimeDigits + 1;
constexpr size_t kTagStart = kLevelIndex + 2;
static_assert(kTagStart == 25, "header layout must match the formatter");

// Shortest valid line: header, 1-char tag, ": ", empty message, '\n'.
constexpr size_t kMinLineLength = kTagStart + 1 + 2 + 1;

constexpr bool is_printable(unsigned char byte) {
    return byte >= 0x20 && byte <= 0x7E;
}

// Reads exactly `count` ASCII digits. Hand-rolled on purpose: strtoul/sscanf accept leading
// spaces, signs and "0x", which must all be rejected here.
bool read_digits(const char* text, size_t count, uint64_t& value) {
    value = 0;
    for (size_t i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return true;
}

}  // namespace

const char* to_string(ParseStatus status) {
    switch (status) {
        case ParseStatus::Ok:
            return "ok";
        case ParseStatus::Torn:
            return "torn";
        case ParseStatus::Garbage:
            return "garbage";
        case ParseStatus::BadHeader:
            return "bad_header";
        case ParseStatus::BadLevel:
            return "bad_level";
        case ParseStatus::Overlong:
            return "overlong";
    }
    return "unknown";
}

ParseStatus parse_line(const char* line, size_t length, LogRecord& out) {
    // The checks run in this order because the order decides the category.
    // 1. The store's sentinel first: a torn tail (at most 169 bytes) closed with DEL + '\n' is
    //    up to 171 bytes and must be reported as torn, not as overlong. A DEL anywhere else is
    //    media corruption and falls through to Garbage below.
    if (length >= 2 && length <= kMaxLineLength + 1 && line[length - 2] == kTornSentinel &&
        line[length - 1] == '\n') {
        return ParseStatus::Torn;
    }
    if (length > kMaxLineLength) {
        return ParseStatus::Overlong;
    }
    if (length == 0 || line[length - 1] != '\n') {
        return ParseStatus::Torn;
    }
    const size_t body_end = length - 1;  // index of the final '\n'
    for (size_t i = 0; i < body_end; ++i) {
        if (!is_printable(static_cast<unsigned char>(line[i]))) {
            return ParseStatus::Garbage;
        }
    }
    if (length < kMinLineLength) {
        return ParseStatus::BadHeader;
    }

    uint64_t boot = 0;
    uint64_t seq = 0;
    uint64_t uptime = 0;
    if (!read_digits(line + kBootStart, kBootDigits, boot) || line[kSeqStart - 1] != ':' ||
        !read_digits(line + kSeqStart, kSeqDigits, seq) || line[kUptimeStart - 1] != ' ' ||
        !read_digits(line + kUptimeStart, kUptimeDigits, uptime) || line[kLevelIndex - 1] != ' ' ||
        line[kLevelIndex + 1] != ' ') {
        return ParseStatus::BadHeader;
    }
    if (uptime > UINT32_MAX) {  // 10 digits allow up to 9999999999
        return ParseStatus::BadHeader;
    }

    LogLevel level = LogLevel::Info;
    if (!level_from_char(line[kLevelIndex], level)) {
        return ParseStatus::BadLevel;
    }

    // Tag: up to the first ':' (the formatter never leaves ':' or space inside a tag).
    size_t colon = kTagStart;
    while (colon < body_end && line[colon] != ':') {
        if (line[colon] == ' ') {
            return ParseStatus::BadHeader;
        }
        ++colon;
    }
    const size_t tag_length = colon - kTagStart;
    if (colon >= body_end || tag_length == 0 || tag_length > kTagCapacity - 1) {
        return ParseStatus::BadHeader;
    }
    if (colon + 1 >= body_end || line[colon + 1] != ' ') {
        return ParseStatus::BadHeader;
    }

    const size_t message_start = colon + 2;
    const size_t message_length = body_end - message_start;
    if (message_length > kMsgCapacity - 1) {
        return ParseStatus::Overlong;  // longer than the formatter can emit, like length > 170
    }

    // Every rejection returned above, so writing `out` directly keeps "only written on Ok"
    // without a 160-byte temporary on the stack.
    out.boot = static_cast<uint32_t>(boot);
    out.seq = static_cast<uint32_t>(seq);
    out.uptime_ms = static_cast<uint32_t>(uptime);
    out.level = level;
    std::memcpy(out.tag, line + kTagStart, tag_length);
    out.tag[tag_length] = '\0';
    std::memcpy(out.msg, line + message_start, message_length);
    out.msg[message_length] = '\0';
    out.truncated = message_length == kMsgCapacity - 1 &&
                    std::memcmp(out.msg + message_length - kTruncationMarkerLength,
                                kTruncationMarker, kTruncationMarkerLength) == 0;
    return ParseStatus::Ok;
}

}  // namespace logcat
