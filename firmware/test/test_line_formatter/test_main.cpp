#include <unity.h>

#include <cstring>

#include "logcat/line_formatter.hpp"
#include "logcat/log_record.hpp"

using logcat::LogLevel;
using logcat::LogRecord;

namespace {

// Header "BBBB:SSSSSS UUUUUUUUUU L " is 25 chars; the level letter sits at index 23.
constexpr size_t kLevelIndex = 23;
constexpr size_t kTagIndex = 25;

char line[logcat::kLineBufferSize];

LogRecord make_record(uint32_t boot, uint32_t seq, uint32_t uptime_ms, LogLevel level,
                      const char* tag, const char* msg) {
    LogRecord record{};
    record.boot = boot;
    record.seq = seq;
    record.uptime_ms = uptime_ms;
    record.level = level;
    record.set_tag(tag);
    record.set_message(msg);
    return record;
}

size_t count_char(const char* text, char wanted) {
    size_t count = 0;
    for (; *text != '\0'; ++text) {
        count += (*text == wanted) ? 1 : 0;
    }
    return count;
}

}  // namespace

void setUp() {
    std::memset(line, '#', sizeof(line));
}
void tearDown() {}

void test_max_line_length_is_170() {
    TEST_ASSERT_EQUAL_size_t(170, logcat::kMaxLineLength);
    TEST_ASSERT_EQUAL_size_t(171, sizeof(line));
}

void test_formats_nominal_line() {
    const LogRecord record =
        make_record(42, 137, 123456, LogLevel::Warn, "PRESSURE", "reading out of range (4.71 V)");
    const size_t length = logcat::format_line(record, line);
    TEST_ASSERT_EQUAL_STRING("0042:000137 0000123456 W PRESSURE: reading out of range (4.71 V)\n",
                             line);
    TEST_ASSERT_EQUAL_size_t(std::strlen(line), length);
}

void test_zero_fields_are_zero_padded() {
    logcat::format_line(make_record(0, 0, 0, LogLevel::Error, "T", "m"), line);
    TEST_ASSERT_EQUAL_STRING("0000:000000 0000000000 E T: m\n", line);
}

// The boot field has 4 digits; a board in a boot loop passes 9999 in about a day.
void test_boot_prints_modulo_10000() {
    logcat::format_line(make_record(9999, 1, 1, LogLevel::Info, "A", "x"), line);
    TEST_ASSERT_EQUAL_STRING_LEN("9999:", line, 5);
    logcat::format_line(make_record(10000, 1, 1, LogLevel::Info, "A", "x"), line);
    TEST_ASSERT_EQUAL_STRING_LEN("0000:", line, 5);
    logcat::format_line(make_record(0xFFFFFFFF, 1, 1, LogLevel::Info, "A", "x"), line);
    TEST_ASSERT_EQUAL_STRING_LEN("7295:", line, 5);
}

// The Sequencer never hands out seq >= 1e6, but a record built by hand or read back from RTC
// memory might carry one; the field must keep its width.
void test_seq_out_of_range_prints_modulo() {
    LogRecord record = make_record(1, 0, 1, LogLevel::Info, "A", "x");
    record.seq = 1'000'123;
    logcat::format_line(record, line);
    TEST_ASSERT_EQUAL_STRING_LEN("0001:000123 ", line, 12);
}

void test_uptime_uses_full_32_bit_range() {
    logcat::format_line(make_record(1, 1, 0xFFFFFFFF, LogLevel::Info, "A", "x"), line);
    TEST_ASSERT_EQUAL_STRING_LEN("0001:000001 4294967295 I ", line, 25);
}

void test_every_level_letter() {
    const LogLevel levels[] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug,
                               LogLevel::Verbose};
    const char letters[] = {'E', 'W', 'I', 'D', 'V'};
    for (size_t i = 0; i < 5; ++i) {
        logcat::format_line(make_record(1, 1, 1, levels[i], "A", "x"), line);
        TEST_ASSERT_EQUAL_CHAR(letters[i], line[kLevelIndex]);
    }
}

// An invalid level is a bug upstream: print '?' so the parser rejects the line and the host
// shows it raw instead of silently inventing a level.
void test_invalid_level_prints_question_mark() {
    logcat::format_line(make_record(1, 1, 1, static_cast<LogLevel>(0), "A", "x"), line);
    TEST_ASSERT_EQUAL_CHAR('?', line[kLevelIndex]);
    logcat::format_line(make_record(1, 1, 1, static_cast<LogLevel>(99), "A", "x"), line);
    TEST_ASSERT_EQUAL_CHAR('?', line[kLevelIndex]);
}

void test_tag_of_15_chars_is_kept() {
    logcat::format_line(make_record(1, 1, 1, LogLevel::Info, "ABCDEFGHIJKLMNO", "x"), line);
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNO: x\n", line + kTagIndex);
}

void test_empty_tag_prints_dash() {
    logcat::format_line(make_record(42, 137, 123456, LogLevel::Info, "", "hello"), line);
    TEST_ASSERT_EQUAL_STRING("0042:000137 0000123456 I -: hello\n", line);
}

// "TAG: msg" must split unambiguously at the first ": ".
void test_tag_space_and_colon_become_underscore() {
    logcat::format_line(make_record(1, 1, 1, LogLevel::Info, "A B:C", "x"), line);
    TEST_ASSERT_EQUAL_STRING("A_B_C: x\n", line + kTagIndex);
}

void test_tag_control_and_non_ascii_become_question_mark() {
    logcat::format_line(make_record(1, 1, 1, LogLevel::Info,
                                    "A\nB"
                                    "\xFF",
                                    "x"),
                        line);
    TEST_ASSERT_EQUAL_STRING("A?B?: x\n", line + kTagIndex);
}

void test_empty_message_keeps_separator() {
    logcat::format_line(make_record(1, 1, 1, LogLevel::Info, "APP", ""), line);
    TEST_ASSERT_EQUAL_STRING("APP: \n", line + kTagIndex);
}

// ADR-006: a newline inside a message must never produce a second (forged) line.
void test_message_control_and_non_ascii_become_question_mark() {
    logcat::format_line(make_record(1, 1, 1, LogLevel::Info, "APP",
                                    "a\nb\rc\td"
                                    "\xFF"
                                    "e"
                                    "\xC3\xA3"),
                        line);
    TEST_ASSERT_EQUAL_STRING("APP: a?b?c?d?e??\n", line + kTagIndex);
    TEST_ASSERT_EQUAL_size_t(1, count_char(line, '\n'));
}

void test_forged_line_in_message_stays_one_line() {
    logcat::format_line(make_record(1, 1, 1, LogLevel::Error, "APP",
                                    "boom\n0042:000001 0000000001 E FAKE: injected"),
                        line);
    TEST_ASSERT_EQUAL_STRING("APP: boom?0042:000001 0000000001 E FAKE: injected\n",
                             line + kTagIndex);
    TEST_ASSERT_EQUAL_size_t(1, count_char(line, '\n'));
}

void test_truncated_message_shows_124_chars_and_marker() {
    char text[logcat::kMsgCapacity + 1];
    std::memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    const LogRecord record = make_record(1, 1, 1, LogLevel::Info, "APP", text);
    TEST_ASSERT_TRUE(record.truncated);

    const size_t length = logcat::format_line(record, line);
    const char* message = line + kTagIndex + std::strlen("APP: ");
    TEST_ASSERT_EQUAL_size_t(124 + 3 + 1, std::strlen(message));  // 124 chars + "..." + '\n'
    TEST_ASSERT_EQUAL_size_t(124, std::strspn(message, "x"));
    TEST_ASSERT_EQUAL_STRING("...\n", message + 124);
    TEST_ASSERT_EQUAL_size_t(std::strlen(line), length);
}

void test_truncated_short_message_appends_marker() {
    LogRecord record = make_record(1, 1, 1, LogLevel::Info, "APP", "abc");
    record.truncated = true;
    logcat::format_line(record, line);
    TEST_ASSERT_EQUAL_STRING("APP: abc...\n", line + kTagIndex);
}

void test_worst_case_line_is_max_length() {
    char text[logcat::kMsgCapacity];
    std::memset(text, 'y', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    const LogRecord full =
        make_record(9999, 999999, 0xFFFFFFFF, LogLevel::Verbose, "ABCDEFGHIJKLMNO", text);
    TEST_ASSERT_FALSE(full.truncated);
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxLineLength, logcat::format_line(full, line));

    LogRecord truncated = full;
    truncated.truncated = true;
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxLineLength, logcat::format_line(truncated, line));
    TEST_ASSERT_EQUAL_CHAR('\0', line[logcat::kMaxLineLength]);
}

// A record read back from RTC memory (M6) may have no terminators at all.
void test_unterminated_tag_and_message_are_bounded() {
    LogRecord record{};
    record.boot = 1;
    record.seq = 1;
    record.uptime_ms = 1;
    record.level = LogLevel::Info;
    std::memset(record.tag, 'T', sizeof(record.tag));
    std::memset(record.msg, 'M', sizeof(record.msg));

    const size_t length = logcat::format_line(record, line);
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxLineLength, length);
    TEST_ASSERT_EQUAL_size_t(15, std::strspn(line + kTagIndex, "T"));
    TEST_ASSERT_EQUAL_size_t(127, std::strspn(line + kTagIndex + 15 + 2, "M"));
    TEST_ASSERT_EQUAL_CHAR('\n', line[length - 1]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_max_line_length_is_170);
    RUN_TEST(test_formats_nominal_line);
    RUN_TEST(test_zero_fields_are_zero_padded);
    RUN_TEST(test_boot_prints_modulo_10000);
    RUN_TEST(test_seq_out_of_range_prints_modulo);
    RUN_TEST(test_uptime_uses_full_32_bit_range);
    RUN_TEST(test_every_level_letter);
    RUN_TEST(test_invalid_level_prints_question_mark);
    RUN_TEST(test_tag_of_15_chars_is_kept);
    RUN_TEST(test_empty_tag_prints_dash);
    RUN_TEST(test_tag_space_and_colon_become_underscore);
    RUN_TEST(test_tag_control_and_non_ascii_become_question_mark);
    RUN_TEST(test_empty_message_keeps_separator);
    RUN_TEST(test_message_control_and_non_ascii_become_question_mark);
    RUN_TEST(test_forged_line_in_message_stays_one_line);
    RUN_TEST(test_truncated_message_shows_124_chars_and_marker);
    RUN_TEST(test_truncated_short_message_appends_marker);
    RUN_TEST(test_worst_case_line_is_max_length);
    RUN_TEST(test_unterminated_tag_and_message_are_bounded);
    return UNITY_END();
}
