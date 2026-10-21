#include <unity.h>

#include <cstring>

#include "logcat/line_formatter.hpp"
#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"

using logcat::LogLevel;
using logcat::LogRecord;
using logcat::ParseStatus;

// Compare by name so a failure reads "Expected 'Torn' Was 'Garbage'".
#define ASSERT_STATUS(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(logcat::to_string(expected), logcat::to_string(actual))

namespace {

constexpr char kNominal[] = "0042:000137 0000123456 W PRESSURE: reading out of range (4.71 V)\n";
constexpr unsigned char kUntouched = 0x5A;

char buffer[256];

uint32_t next_random(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

// Parses `length` bytes, asserts the status and, for any failure, that `out` was not touched.
LogRecord expect_status(ParseStatus expected, const char* line, size_t length) {
    LogRecord out;
    std::memset(&out, kUntouched, sizeof(out));
    LogRecord before;
    std::memcpy(&before, &out, sizeof(out));

    ASSERT_STATUS(expected, logcat::parse_line(line, length, out));
    if (expected != ParseStatus::Ok) {
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &out, sizeof(out), "out modified on failure");
        return LogRecord{};  // `out` holds 0x5A bytes, including an invalid bool: never copy it
    }
    return out;
}

LogRecord expect_status(ParseStatus expected, const char* line) {
    return expect_status(expected, line, std::strlen(line));
}

// Copies kNominal into buffer and replaces one byte.
const char* nominal_with(size_t index, char replacement) {
    std::memcpy(buffer, kNominal, sizeof(kNominal));
    buffer[index] = replacement;
    return buffer;
}

// Builds "<header>TAG: <msg_length × fill>\n" into buffer.
const char* line_with(const char* header, const char* tag, size_t msg_length, char fill) {
    char* cursor = buffer;
    for (const char* p = header; *p != '\0'; ++p)
        *cursor++ = *p;
    for (const char* p = tag; *p != '\0'; ++p)
        *cursor++ = *p;
    *cursor++ = ':';
    *cursor++ = ' ';
    for (size_t i = 0; i < msg_length; ++i)
        *cursor++ = fill;
    *cursor++ = '\n';
    *cursor = '\0';
    return buffer;
}

void assert_same_record(const LogRecord& expected, const LogRecord& actual) {
    TEST_ASSERT_EQUAL_UINT32(expected.boot, actual.boot);
    TEST_ASSERT_EQUAL_UINT32(expected.seq, actual.seq);
    TEST_ASSERT_EQUAL_UINT32(expected.uptime_ms, actual.uptime_ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected.level),
                            static_cast<uint8_t>(actual.level));
    TEST_ASSERT_EQUAL_STRING(expected.tag, actual.tag);
    TEST_ASSERT_EQUAL_STRING(expected.msg, actual.msg);
    TEST_ASSERT_EQUAL(expected.truncated, actual.truncated);
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- valid lines

void test_parses_nominal_line() {
    const LogRecord record = expect_status(ParseStatus::Ok, kNominal);
    TEST_ASSERT_EQUAL_UINT32(42, record.boot);
    TEST_ASSERT_EQUAL_UINT32(137, record.seq);
    TEST_ASSERT_EQUAL_UINT32(123456, record.uptime_ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LogLevel::Warn),
                            static_cast<uint8_t>(record.level));
    TEST_ASSERT_EQUAL_STRING("PRESSURE", record.tag);
    TEST_ASSERT_EQUAL_STRING("reading out of range (4.71 V)", record.msg);
    TEST_ASSERT_FALSE(record.truncated);
}

void test_parses_maximum_field_values() {
    line_with("9999:999999 4294967295 V ", "ABCDEFGHIJKLMNO", 127, 'y');
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxLineLength, std::strlen(buffer));
    const LogRecord record = expect_status(ParseStatus::Ok, buffer);
    TEST_ASSERT_EQUAL_UINT32(9999, record.boot);
    TEST_ASSERT_EQUAL_UINT32(999999, record.seq);
    TEST_ASSERT_EQUAL_UINT32(4294967295u, record.uptime_ms);
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNO", record.tag);
    TEST_ASSERT_EQUAL_size_t(127, std::strlen(record.msg));
    TEST_ASSERT_FALSE(record.truncated);
}

void test_parses_empty_message() {
    const LogRecord record = expect_status(ParseStatus::Ok, "0001:000001 0000000001 I APP: \n");
    TEST_ASSERT_EQUAL_STRING("APP", record.tag);
    TEST_ASSERT_EQUAL_STRING("", record.msg);
}

// The formatter prints '-' for an empty tag; the parser cannot tell, so it returns '-'.
void test_dash_tag_is_returned_as_dash() {
    const LogRecord record = expect_status(ParseStatus::Ok, "0001:000001 0000000001 I -: x\n");
    TEST_ASSERT_EQUAL_STRING("-", record.tag);
}

// Messages are data: edge spaces and header-looking text (a log inside a log) are kept as is.
void test_message_keeps_edge_spaces_and_header_like_text() {
    const LogRecord record = expect_status(
        ParseStatus::Ok, "0001:000001 0000000001 I APP:   0042:000001 0000000001 E FAKE: x  \n");
    TEST_ASSERT_EQUAL_STRING("  0042:000001 0000000001 E FAKE: x  ", record.msg);
}

void test_truncated_line_sets_flag_and_keeps_marker() {
    line_with("0001:000001 0000000001 I ", "APP", 124, 'x');
    std::memcpy(buffer + std::strlen(buffer) - 1, "...\n", 5);
    const LogRecord record = expect_status(ParseStatus::Ok, buffer);
    TEST_ASSERT_TRUE(record.truncated);
    TEST_ASSERT_EQUAL_size_t(127, std::strlen(record.msg));
    TEST_ASSERT_EQUAL_STRING("...", record.msg + 124);
}

void test_127_chars_without_marker_is_not_truncated() {
    line_with("0001:000001 0000000001 I ", "APP", 127, 'x');
    TEST_ASSERT_FALSE(expect_status(ParseStatus::Ok, buffer).truncated);
}

void test_short_message_ending_in_dots_is_not_truncated() {
    TEST_ASSERT_FALSE(
        expect_status(ParseStatus::Ok, "0001:000001 0000000001 I APP: wait...\n").truncated);
}

// ---------------------------------------------------------------- round trips

void test_parse_of_format_returns_original_record() {
    const LogLevel levels[] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug,
                               LogLevel::Verbose};
    const char* tags[] = {"A", "PRESSURE", "ABCDEFGHIJKLMNO", "esp_netif"};
    const char* messages[] = {"", "x", "reading out of range (4.71 V)", "  spaced  ",
                              "{json: [1, 2]} ~!@#$%^&*()"};
    uint32_t n = 0;
    for (LogLevel level : levels) {
        for (const char* tag : tags) {
            for (const char* msg : messages) {
                LogRecord original{};
                original.boot = (n * 7919u) % logcat::kBootModulo;
                original.seq = (n * 104729u) % logcat::kSeqModulo;
                original.uptime_ms = n * 2654435761u;
                original.level = level;
                original.set_tag(tag);
                original.set_message(msg);
                ++n;

                char line[logcat::kLineBufferSize];
                const size_t length = logcat::format_line(original, line);
                assert_same_record(original, expect_status(ParseStatus::Ok, line, length));
            }
        }
    }
}

void test_format_of_parse_reproduces_valid_line() {
    line_with("0007:000123 0000004567 E ", "APP", 124, 'z');
    std::memcpy(buffer + std::strlen(buffer) - 1, "...\n", 5);
    static char truncated_line[logcat::kLineBufferSize];
    std::memcpy(truncated_line, buffer, std::strlen(buffer) + 1);

    const char* lines[] = {kNominal, "0000:000000 0000000000 D -: \n",
                           "9999:999999 4294967295 V ABCDEFGHIJKLMNO: ~ end with space \n",
                           truncated_line};
    for (const char* original : lines) {
        const LogRecord record = expect_status(ParseStatus::Ok, original);
        char line[logcat::kLineBufferSize];
        logcat::format_line(record, line);
        TEST_ASSERT_EQUAL_STRING(original, line);
    }
}

// ---------------------------------------------------------------- torn and garbage

void test_rejects_line_without_newline_as_torn() {
    expect_status(ParseStatus::Torn, kNominal, std::strlen(kNominal) - 1);
}

void test_rejects_empty_input_as_torn() {
    expect_status(ParseStatus::Torn, "", 0);
}

// The store (#23) closes a torn tail with DEL + '\n' on reopen.
void test_rejects_del_sentinel_as_torn() {
    expect_status(ParseStatus::Torn, "0042:000137 0000123456 W PRESSURE: readi\x7F\n");
}

void test_rejects_crlf_as_garbage() {
    nominal_with(std::strlen(kNominal) - 1, '\r');
    std::strcat(buffer, "\n");
    expect_status(ParseStatus::Garbage, buffer);
}

void test_rejects_embedded_nul_and_high_bytes_as_garbage() {
    const char bad[] = {'\0', '\x01', '\x1F', '\x80', '\xFF'};
    for (char c : bad) {
        nominal_with(40, c);
        expect_status(ParseStatus::Garbage, buffer, std::strlen(kNominal));
    }
}

void test_rejects_inner_newline_as_garbage() {
    expect_status(ParseStatus::Garbage, nominal_with(40, '\n'));
}

void test_rejects_erased_flash_as_garbage() {
    char erased[26];
    std::memset(erased, '\xFF', 25);
    erased[25] = '\n';
    expect_status(ParseStatus::Garbage, erased, sizeof(erased));
}

void test_rejects_erased_flash_without_newline_as_torn() {
    char erased[25];
    std::memset(erased, '\xFF', sizeof(erased));
    expect_status(ParseStatus::Torn, erased, sizeof(erased));
}

// ---------------------------------------------------------------- length

void test_rejects_line_over_170_bytes_as_overlong() {
    line_with("0001:000001 0000000001 I ", "ABCDEFGHIJKLMNO", 128, 'y');
    TEST_ASSERT_EQUAL_size_t(171, std::strlen(buffer));
    expect_status(ParseStatus::Overlong, buffer);
}

// A torn 169-byte tail plus DEL + '\n' is 171 bytes: still torn, not overlong.
void test_closed_torn_tail_of_max_length_is_torn() {
    line_with("0001:000001 0000000001 I ", "ABCDEFGHIJKLMNO", 127, 'y');
    buffer[169] = '\x7F';  // bytes 0..168 are the 169-byte torn tail
    buffer[170] = '\n';
    expect_status(ParseStatus::Torn, buffer, 171);
}

void test_rejects_line_shorter_than_29_bytes() {
    expect_status(ParseStatus::BadHeader, "0001:000001 0000000001 I : \n");
}

// ---------------------------------------------------------------- header

void test_rejects_each_broken_header_position() {
    for (size_t index = 0; index < 25; ++index) {
        if (index == 23) {
            continue;  // level letter: covered by BadLevel
        }
        const char wrong = (kNominal[index] >= '0' && kNominal[index] <= '9') ? 'x' : '_';
        expect_status(ParseStatus::BadHeader, nominal_with(index, wrong));
    }
}

// strtoul/sscanf would accept these.
void test_rejects_sign_space_and_hex_in_numbers() {
    expect_status(ParseStatus::BadHeader, "+042:000137 0000123456 W A: x\n");
    expect_status(ParseStatus::BadHeader, " 042:000137 0000123456 W A: x\n");
    expect_status(ParseStatus::BadHeader, "0x42:000137 0000123456 W A: x\n");
    expect_status(ParseStatus::BadHeader, "0042:-00137 0000123456 W A: x\n");
}

void test_rejects_uptime_above_32_bits() {
    expect_status(ParseStatus::BadHeader, "0001:000001 4294967296 I A: x\n");
    expect_status(ParseStatus::BadHeader, "0001:000001 9999999999 I A: x\n");
}

void test_rejects_invalid_level() {
    expect_status(ParseStatus::BadLevel, nominal_with(23, '?'));
    expect_status(ParseStatus::BadLevel, nominal_with(23, 'e'));
    expect_status(ParseStatus::BadLevel, nominal_with(23, 'X'));
}

// ---------------------------------------------------------------- tag and message

void test_rejects_bad_tags() {
    expect_status(ParseStatus::BadHeader, "0001:000001 0000000001 I : message\n");
    line_with("0001:000001 0000000001 I ", "ABCDEFGHIJKLMNOP", 1, 'x');
    expect_status(ParseStatus::BadHeader, buffer);
    expect_status(ParseStatus::BadHeader, "0001:000001 0000000001 I A B: x\n");
    expect_status(ParseStatus::BadHeader, "0001:000001 0000000001 I APP:x\n");
    expect_status(ParseStatus::BadHeader, "0001:000001 0000000001 I APP no colon\n");
    expect_status(ParseStatus::BadHeader, "0001:000001 0000000001 I APP:\n");
}

void test_rejects_message_over_127_chars() {
    line_with("0001:000001 0000000001 I ", "A", 128, 'm');
    expect_status(ParseStatus::BadHeader, buffer);
}

void test_to_string_names_every_status() {
    TEST_ASSERT_EQUAL_STRING("ok", logcat::to_string(ParseStatus::Ok));
    TEST_ASSERT_EQUAL_STRING("torn", logcat::to_string(ParseStatus::Torn));
    TEST_ASSERT_EQUAL_STRING("garbage", logcat::to_string(ParseStatus::Garbage));
    TEST_ASSERT_EQUAL_STRING("bad_header", logcat::to_string(ParseStatus::BadHeader));
    TEST_ASSERT_EQUAL_STRING("bad_level", logcat::to_string(ParseStatus::BadLevel));
    TEST_ASSERT_EQUAL_STRING("overlong", logcat::to_string(ParseStatus::Overlong));
}

// ---------------------------------------------------------------- properties

// Any record the formatter can emit parses back (or is rejected for its level only), and
// re-formatting the parsed record reproduces the line byte for byte.
void test_random_formatted_lines_round_trip() {
    uint32_t state = 2024;
    for (int iteration = 0; iteration < 5000; ++iteration) {
        LogRecord record{};
        record.boot = next_random(state) * 977u;
        record.seq = next_random(state) * 31u;
        record.uptime_ms = next_random(state) * 263u;
        const unsigned char level_byte = static_cast<unsigned char>(next_random(state) % 8);
        std::memcpy(&record.level, &level_byte, 1);
        const unsigned char flag_byte = static_cast<unsigned char>(next_random(state));
        std::memcpy(&record.truncated, &flag_byte, 1);
        for (char& c : record.tag)
            c = static_cast<char>(next_random(state));
        for (char& c : record.msg)
            c = static_cast<char>(next_random(state));

        char line[logcat::kLineBufferSize];
        const size_t length = logcat::format_line(record, line);
        const bool valid_level = level_byte >= 1 && level_byte <= 5;
        const LogRecord parsed =
            expect_status(valid_level ? ParseStatus::Ok : ParseStatus::BadLevel, line, length);
        if (valid_level) {
            char again[logcat::kLineBufferSize];
            logcat::format_line(parsed, again);
            TEST_ASSERT_EQUAL_STRING(line, again);
        }
    }
}

// Power can fail after any byte of a write. Every prefix must be torn, never a record, and
// so must every prefix closed by the store's DEL + '\n' sentinel.
void test_every_torn_prefix_is_rejected() {
    line_with("9999:999999 4294967295 V ", "ABCDEFGHIJKLMNO", 127, 'y');
    char full[logcat::kLineBufferSize];
    std::memcpy(full, buffer, sizeof(full));
    const char* lines[] = {kNominal, full};

    for (const char* line : lines) {
        const size_t length = std::strlen(line);
        for (size_t cut = 0; cut < length; ++cut) {
            expect_status(ParseStatus::Torn, line, cut);

            char closed[logcat::kMaxLineLength + 2];
            std::memcpy(closed, line, cut);
            closed[cut] = '\x7F';
            closed[cut + 1] = '\n';
            expect_status(ParseStatus::Torn, closed, cut + 2);
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parses_nominal_line);
    RUN_TEST(test_parses_maximum_field_values);
    RUN_TEST(test_parses_empty_message);
    RUN_TEST(test_dash_tag_is_returned_as_dash);
    RUN_TEST(test_message_keeps_edge_spaces_and_header_like_text);
    RUN_TEST(test_truncated_line_sets_flag_and_keeps_marker);
    RUN_TEST(test_127_chars_without_marker_is_not_truncated);
    RUN_TEST(test_short_message_ending_in_dots_is_not_truncated);
    RUN_TEST(test_parse_of_format_returns_original_record);
    RUN_TEST(test_format_of_parse_reproduces_valid_line);
    RUN_TEST(test_rejects_line_without_newline_as_torn);
    RUN_TEST(test_rejects_empty_input_as_torn);
    RUN_TEST(test_rejects_del_sentinel_as_torn);
    RUN_TEST(test_rejects_crlf_as_garbage);
    RUN_TEST(test_rejects_embedded_nul_and_high_bytes_as_garbage);
    RUN_TEST(test_rejects_inner_newline_as_garbage);
    RUN_TEST(test_rejects_erased_flash_as_garbage);
    RUN_TEST(test_rejects_erased_flash_without_newline_as_torn);
    RUN_TEST(test_rejects_line_over_170_bytes_as_overlong);
    RUN_TEST(test_closed_torn_tail_of_max_length_is_torn);
    RUN_TEST(test_rejects_line_shorter_than_29_bytes);
    RUN_TEST(test_rejects_each_broken_header_position);
    RUN_TEST(test_rejects_sign_space_and_hex_in_numbers);
    RUN_TEST(test_rejects_uptime_above_32_bits);
    RUN_TEST(test_rejects_invalid_level);
    RUN_TEST(test_rejects_bad_tags);
    RUN_TEST(test_rejects_message_over_127_chars);
    RUN_TEST(test_to_string_names_every_status);
    RUN_TEST(test_random_formatted_lines_round_trip);
    RUN_TEST(test_every_torn_prefix_is_rejected);
    return UNITY_END();
}
