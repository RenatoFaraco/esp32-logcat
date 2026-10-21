#include <unity.h>

#include <cstring>
#include <type_traits>

#include "logcat/log_record.hpp"

using logcat::LogLevel;
using logcat::LogRecord;

void setUp() {}
void tearDown() {}

// The FreeRTOS queue copies items with memcpy and holds 32 of them (ARCHITECTURE §8).
void test_log_record_fits_queue_budget() {
    TEST_ASSERT_LESS_OR_EQUAL(160, sizeof(LogRecord));
    TEST_ASSERT_TRUE(std::is_trivially_copyable_v<LogRecord>);
}

void test_level_to_char_maps_every_level() {
    TEST_ASSERT_EQUAL_CHAR('E', logcat::level_to_char(LogLevel::Error));
    TEST_ASSERT_EQUAL_CHAR('W', logcat::level_to_char(LogLevel::Warn));
    TEST_ASSERT_EQUAL_CHAR('I', logcat::level_to_char(LogLevel::Info));
    TEST_ASSERT_EQUAL_CHAR('D', logcat::level_to_char(LogLevel::Debug));
    TEST_ASSERT_EQUAL_CHAR('V', logcat::level_to_char(LogLevel::Verbose));
}

void test_level_from_char_round_trips() {
    const LogLevel levels[] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug,
                               LogLevel::Verbose};
    for (LogLevel expected : levels) {
        LogLevel parsed = LogLevel::Info;
        TEST_ASSERT_TRUE(logcat::level_from_char(logcat::level_to_char(expected), parsed));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(parsed));
    }
}

void test_level_from_char_rejects_unknown() {
    const char invalid[] = {'X', 'e', ' ', '\0', static_cast<char>(0xFF)};
    for (char c : invalid) {
        LogLevel parsed = LogLevel::Debug;
        TEST_ASSERT_FALSE(logcat::level_from_char(c, parsed));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LogLevel::Debug),
                                static_cast<uint8_t>(parsed));
    }
}

// A zero-initialized or corrupted record must not print a valid-looking level letter.
void test_level_to_char_out_of_range_is_question_mark() {
    TEST_ASSERT_EQUAL_CHAR('?', logcat::level_to_char(static_cast<LogLevel>(0)));
    TEST_ASSERT_EQUAL_CHAR('?', logcat::level_to_char(static_cast<LogLevel>(99)));
}

void test_set_tag_copies_short_tag() {
    LogRecord record{};
    record.set_tag("PRESSURE");
    TEST_ASSERT_EQUAL_STRING("PRESSURE", record.tag);
}

void test_set_tag_truncates_to_15_chars() {
    LogRecord record{};
    record.set_tag("ABCDEFGHIJKLMNOPQRS");
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNO", record.tag);
    TEST_ASSERT_FALSE(record.truncated);  // only the message sets the flag
}

void test_set_tag_keeps_exactly_15_chars() {
    LogRecord record{};
    record.set_tag("ABCDEFGHIJKLMNO");
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNO", record.tag);
}

void test_set_tag_null_is_empty() {
    LogRecord record{};
    record.set_tag("OLD");
    record.set_tag(nullptr);
    TEST_ASSERT_EQUAL_STRING("", record.tag);
}

void test_set_message_fits_exactly_127_chars_without_truncation() {
    char text[logcat::kMsgCapacity];
    std::memset(text, 'a', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    LogRecord record{};
    record.set_message(text);
    TEST_ASSERT_EQUAL_size_t(127, std::strlen(record.msg));
    TEST_ASSERT_FALSE(record.truncated);
}

void test_set_message_truncates_at_128_chars_and_marks_truncated() {
    char text[logcat::kMsgCapacity + 1];
    std::memset(text, 'b', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    LogRecord record{};
    record.set_message(text);
    TEST_ASSERT_EQUAL_size_t(127, std::strlen(record.msg));
    TEST_ASSERT_EQUAL_CHAR('b', record.msg[126]);
    TEST_ASSERT_TRUE(record.truncated);
}

// Input with no terminator within the capacity: copy must stop at the bound, not run on.
void test_set_message_handles_unterminated_input_of_capacity_length() {
    char text[logcat::kMsgCapacity];
    std::memset(text, 'd', sizeof(text));  // no '\0' anywhere

    LogRecord record{};
    record.set_message(text);
    TEST_ASSERT_EQUAL_size_t(127, std::strlen(record.msg));
    TEST_ASSERT_TRUE(record.truncated);
}

void test_set_message_clears_previous_truncated_flag() {
    char text[logcat::kMsgCapacity * 2];
    std::memset(text, 'c', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    LogRecord record{};
    record.set_message(text);
    record.set_message("ok");
    TEST_ASSERT_EQUAL_STRING("ok", record.msg);
    TEST_ASSERT_FALSE(record.truncated);
}

void test_set_message_null_is_empty() {
    LogRecord record{};
    record.set_message("old");
    record.set_message(nullptr);
    TEST_ASSERT_EQUAL_STRING("", record.msg);
    TEST_ASSERT_FALSE(record.truncated);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_log_record_fits_queue_budget);
    RUN_TEST(test_level_to_char_maps_every_level);
    RUN_TEST(test_level_from_char_round_trips);
    RUN_TEST(test_level_from_char_rejects_unknown);
    RUN_TEST(test_level_to_char_out_of_range_is_question_mark);
    RUN_TEST(test_set_tag_copies_short_tag);
    RUN_TEST(test_set_tag_truncates_to_15_chars);
    RUN_TEST(test_set_tag_keeps_exactly_15_chars);
    RUN_TEST(test_set_tag_null_is_empty);
    RUN_TEST(test_set_message_fits_exactly_127_chars_without_truncation);
    RUN_TEST(test_set_message_truncates_at_128_chars_and_marks_truncated);
    RUN_TEST(test_set_message_handles_unterminated_input_of_capacity_length);
    RUN_TEST(test_set_message_clears_previous_truncated_flag);
    RUN_TEST(test_set_message_null_is_empty);
    return UNITY_END();
}
