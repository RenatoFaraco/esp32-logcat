#include "logcat/idf_text.hpp"

namespace logcat {

namespace {

constexpr size_t kMaxTimestampDigits = 10;  // uint32_t milliseconds

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Skips one ANSI escape sequence ("\x1b[...<letter>") starting at `pos`; returns the position
// after it, or `pos` if there is none.
size_t skip_ansi(const char* text, size_t length, size_t pos) {
    if (pos + 1 >= length || text[pos] != '\x1b' || text[pos + 1] != '[') {
        return pos;
    }
    for (size_t i = pos + 2; i < length; ++i) {
        const char c = text[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            return i + 1;
        }
    }
    return length;  // unterminated sequence: nothing left to parse
}

}  // namespace

bool parse_idf_text(const char* text, size_t length, IdfText& out) {
    size_t pos = skip_ansi(text, length, 0);

    // "L (" — level letter, space, parenthesis.
    if (pos + 3 > length) {
        return false;
    }
    LogLevel level = LogLevel::Info;
    if (!level_from_char(text[pos], level) || text[pos + 1] != ' ' || text[pos + 2] != '(') {
        return false;
    }
    pos += 3;

    // "timestamp) "
    const size_t digits_start = pos;
    while (pos < length && is_digit(text[pos])) {
        ++pos;
    }
    const size_t digits = pos - digits_start;
    if (digits == 0 || digits > kMaxTimestampDigits) {
        return false;
    }
    if (pos + 2 > length || text[pos] != ')' || text[pos + 1] != ' ') {
        return false;
    }
    pos += 2;

    // "TAG: " — the tag ends at the first ": ".
    const size_t tag_start = pos;
    while (pos + 1 < length && !(text[pos] == ':' && text[pos + 1] == ' ')) {
        if (text[pos] == '\n') {
            return false;
        }
        ++pos;
    }
    if (pos + 1 >= length || pos == tag_start) {
        return false;
    }
    const size_t tag_length = pos - tag_start;
    pos += 2;

    // Message: the rest, without the final '\n' and a trailing ANSI reset.
    size_t end = length;
    if (end > pos && text[end - 1] == '\n') {
        --end;
    }
    constexpr char kReset[] = "\x1b[0m";
    constexpr size_t kResetLength = sizeof(kReset) - 1;
    if (end - pos >= kResetLength) {
        bool is_reset = true;
        for (size_t i = 0; i < kResetLength; ++i) {
            is_reset = is_reset && text[end - kResetLength + i] == kReset[i];
        }
        if (is_reset) {
            end -= kResetLength;
        }
    }

    out.level = level;
    out.tag = text + tag_start;
    out.tag_length = tag_length;
    out.message = text + pos;
    out.message_length = end - pos;
    return true;
}

}  // namespace logcat
