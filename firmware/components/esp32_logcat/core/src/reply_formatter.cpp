#include "logcat/reply_formatter.hpp"

#include <cstring>

namespace logcat {

namespace {

constexpr size_t kMaxIdLength = 4;
constexpr size_t kMaxKeyLength = 32;
constexpr size_t kMaxCodeLength = 32;
constexpr size_t kMaxNameLength = 64;
constexpr size_t kMaxBody = kMaxLineLength - 1;  // bytes before the final '\n'
// The fixed parts are written without a length check: these keep that safe.
static_assert(3 + kMaxIdLength + 1 + kMaxCodeLength + 1 < kMaxBody,
              "\"#! <id> <code> \" must leave room for the text");
static_assert(7 + kMaxIdLength + 1 + kMaxNameLength + 1 + 10 <= kMaxBody, "#BEGIN always fits");

constexpr bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}
constexpr bool is_digit(char c) {
    return c >= '0' && c <= '9';
}
constexpr bool is_alnum(char c) {
    return is_lower(c) || is_digit(c) || (c >= 'A' && c <= 'Z');
}
// Printable ASCII without the space: what a value may hold.
constexpr bool is_graphic(char c) {
    return c >= 0x21 && c <= 0x7E;
}

// Length of `text` if it is at most `limit` chars, otherwise limit + 1 (never reads further).
size_t bounded_length(const char* text, size_t limit) {
    size_t length = 0;
    while (length <= limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

// Length of a valid key, or 0.
size_t key_length(const char* key) {
    if (key == nullptr || !is_lower(key[0])) {
        return 0;
    }
    const size_t length = bounded_length(key, kMaxKeyLength);
    if (length > kMaxKeyLength) {
        return 0;
    }
    for (size_t i = 1; i < length; ++i) {
        if (!is_lower(key[i]) && !is_digit(key[i]) && key[i] != '_' && key[i] != '.') {
            return 0;
        }
    }
    return length;
}

// Length of a valid error code, or 0.
size_t code_length(const char* code) {
    if (code == nullptr) {
        return 0;
    }
    const size_t length = bounded_length(code, kMaxCodeLength);
    if (length == 0 || length > kMaxCodeLength || std::strcmp(code, "ok") == 0) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!is_lower(code[i]) && code[i] != '_') {
            return 0;
        }
    }
    return length;
}

// Length of a valid dump name, or 0.
size_t name_length(const char* name) {
    if (name == nullptr || (!is_alnum(name[0]) && name[0] != '_')) {
        return 0;
    }
    const size_t length = bounded_length(name, kMaxNameLength);
    if (length > kMaxNameLength) {
        return 0;
    }
    for (size_t i = 1; i < length; ++i) {
        if (!is_alnum(name[i]) && name[i] != '.' && name[i] != '_' && name[i] != '-') {
            return 0;
        }
    }
    return length;
}

char* put(char* out, const char* text, size_t length) {
    std::memcpy(out, text, length);
    return out + length;
}

// Decimal without leading zeros; returns the number of digits (1 to 10).
size_t write_uint(uint32_t value, char (&out)[10]) {
    char reversed[10];
    size_t count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (size_t i = 0; i < count; ++i) {
        out[i] = reversed[count - 1 - i];
    }
    return count;
}

// Writes "<prefix> <id>" and returns the cursor, or nullptr for an invalid id.
char* begin_line(char* out, const char* prefix, const char* id) {
    if (!is_reply_id(id)) {
        out[0] = '\0';
        return nullptr;
    }
    char* cursor = put(out, prefix, std::strlen(prefix));
    *cursor++ = ' ';
    return put(cursor, id, std::strlen(id));
}

size_t end_line(char* out, char* cursor) {
    *cursor++ = '\n';
    *cursor = '\0';
    return static_cast<size_t>(cursor - out);
}

constexpr uint32_t crc_of_nibble(uint32_t nibble) {
    uint32_t crc = nibble;
    for (int bit = 0; bit < 4; ++bit) {
        crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

struct CrcTable {
    uint32_t entry[16];
};

constexpr CrcTable make_crc_table() {
    CrcTable table{};
    for (uint32_t i = 0; i < 16; ++i) {
        table.entry[i] = crc_of_nibble(i);
    }
    return table;
}

constexpr CrcTable kCrcTable = make_crc_table();
static_assert(kCrcTable.entry[1] == 0x1DB71064u && kCrcTable.entry[15] == 0xBDBDF21Cu,
              "CRC-32 IEEE nibble table");

}  // namespace

bool is_reply_id(const char* id) {
    if (id == nullptr) {
        return false;
    }
    if (id[0] == '-') {
        return id[1] == '\0';
    }
    if (id[0] < '1' || id[0] > '9') {
        return false;
    }
    const size_t length = bounded_length(id, kMaxIdLength);
    if (length > kMaxIdLength) {
        return false;
    }
    for (size_t i = 1; i < length; ++i) {
        if (!is_digit(id[i])) {
            return false;
        }
    }
    return true;
}

ReplyData::ReplyData(char (&line)[kLineBufferSize], const char* id) : line_(line) {
    char* cursor = begin_line(line_, "#>", id);
    length_ = cursor == nullptr ? 0 : static_cast<size_t>(cursor - line_);
}

bool ReplyData::has_key(const char* key, size_t key_length) const {
    // Fields start after each space that follows the id: " key=value".
    size_t i = 3;  // skip "#> "
    while (i < length_ && line_[i] != ' ') {
        ++i;
    }
    while (i < length_) {
        const size_t start = i + 1;  // after the space
        size_t equals = start;
        while (equals < length_ && line_[equals] != '=') {
            ++equals;
        }
        if (equals - start == key_length && std::memcmp(line_ + start, key, key_length) == 0) {
            return true;
        }
        i = equals;
        while (i < length_ && line_[i] != ' ') {
            ++i;
        }
    }
    return false;
}

bool ReplyData::add(const char* key, const char* value) {
    const size_t key_size = key_length(key);
    if (length_ == 0 || key_size == 0 || value == nullptr || value[0] == '\0') {
        return false;
    }
    // Room left for the value after " key="; a longer value cannot fit, so stop reading there.
    if (length_ + 1 + key_size + 1 >= kMaxBody) {
        return false;
    }
    const size_t room = kMaxBody - (length_ + 1 + key_size + 1);
    const size_t value_size = bounded_length(value, room);
    if (value_size > room) {
        return false;
    }
    for (size_t i = 0; i < value_size; ++i) {
        if (!is_graphic(value[i])) {
            return false;
        }
    }
    if (has_key(key, key_size)) {
        return false;
    }
    char* cursor = line_ + length_;
    *cursor++ = ' ';
    cursor = put(cursor, key, key_size);
    *cursor++ = '=';
    cursor = put(cursor, value, value_size);
    length_ = static_cast<size_t>(cursor - line_);
    ++fields_;
    return true;
}

bool ReplyData::add_uint(const char* key, uint32_t value) {
    char digits[10];
    char text[11];
    const size_t count = write_uint(value, digits);
    std::memcpy(text, digits, count);
    text[count] = '\0';
    return add(key, text);
}

size_t ReplyData::finish() {
    if (length_ == 0 || fields_ == 0) {
        line_[0] = '\0';
        return 0;
    }
    return end_line(line_, line_ + length_);
}

size_t format_reply_ok(const char* id, char (&out)[kLineBufferSize]) {
    char* cursor = begin_line(out, "#.", id);
    if (cursor == nullptr) {
        return 0;
    }
    cursor = put(cursor, " ok", 3);
    return end_line(out, cursor);
}

size_t format_reply_error(const char* id, const char* code, const char* text,
                          char (&out)[kLineBufferSize]) {
    const size_t code_size = code_length(code);
    char* cursor = code_size == 0 ? nullptr : begin_line(out, "#!", id);
    if (cursor == nullptr) {
        out[0] = '\0';
        return 0;
    }
    *cursor++ = ' ';
    cursor = put(cursor, code, code_size);
    // The text never costs the reply its terminator: it is made to fit the rules instead.
    while (text != nullptr && *text == ' ') {
        ++text;
    }
    if (text != nullptr && text[0] != '\0') {
        *cursor++ = ' ';
        const char* const limit = out + kMaxBody;
        for (size_t i = 0; text[i] != '\0' && cursor < limit; ++i) {
            *cursor++ = (text[i] >= 0x20 && text[i] <= 0x7E) ? text[i] : '?';
        }
    }
    return end_line(out, cursor);
}

size_t format_dump_begin(const char* id, const char* name, uint32_t bytes,
                         char (&out)[kLineBufferSize]) {
    const size_t name_size = name_length(name);
    char* cursor = name_size == 0 ? nullptr : begin_line(out, "#BEGIN", id);
    if (cursor == nullptr) {
        out[0] = '\0';
        return 0;
    }
    *cursor++ = ' ';
    cursor = put(cursor, name, name_size);
    *cursor++ = ' ';
    char digits[10];
    cursor = put(cursor, digits, write_uint(bytes, digits));
    return end_line(out, cursor);
}

size_t format_dump_end(const char* id, uint32_t crc, char (&out)[kLineBufferSize]) {
    char* cursor = begin_line(out, "#END", id);
    if (cursor == nullptr) {
        return 0;
    }
    *cursor++ = ' ';
    for (int shift = 28; shift >= 0; shift -= 4) {
        *cursor++ = "0123456789abcdef"[(crc >> shift) & 0xFu];
    }
    return end_line(out, cursor);
}

uint32_t crc32_update(uint32_t crc, const void* data, size_t length) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ kCrcTable.entry[crc & 0xFu];
        crc = (crc >> 4) ^ kCrcTable.entry[crc & 0xFu];
    }
    return ~crc;
}

}  // namespace logcat
