#include <unity.h>

#include <cstring>

#include "logcat/idf_text.hpp"
#include "logcat/log_record.hpp"

using logcat::IdfText;
using logcat::LogLevel;

namespace {

bool parse(const char* text, IdfText& out) {
    return logcat::parse_idf_text(text, std::strlen(text), out);
}

void assert_span(const char* expected, const char* data, size_t length) {
    TEST_ASSERT_EQUAL_size_t(std::strlen(expected), length);
    TEST_ASSERT_EQUAL_MEMORY(expected, data, length);
}

}  // namespace

void setUp() {}
void tearDown() {}

// Exactly what ESP-IDF's LOG_FORMAT produces for ESP_LOGI(TAG, "...") with colors off.
void test_parses_nominal_line() {
    IdfText out{};
    TEST_ASSERT_TRUE(parse("I (253) APP: formatted 49 bytes\n", out));
    TEST_ASSERT_EQUAL_CHAR('I', logcat::level_to_char(out.level));
    assert_span("APP", out.tag, out.tag_length);
    assert_span("formatted 49 bytes", out.message, out.message_length);
}

void test_every_level_letter() {
    const char* lines[] = {"E (1) T: m\n", "W (1) T: m\n", "I (1) T: m\n", "D (1) T: m\n",
                           "V (1) T: m\n"};
    const LogLevel levels[] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug,
                               LogLevel::Verbose};
    for (size_t i = 0; i < 5; ++i) {
        IdfText out{};
        TEST_ASSERT_TRUE(parse(lines[i], out));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(levels[i]), static_cast<uint8_t>(out.level));
    }
}

void test_tag_with_dots_and_message_with_colons() {
    IdfText out{};
    TEST_ASSERT_TRUE(parse("I (41) boot.esp32c3: SPI Flash Size : 4MB\n", out));
    assert_span("boot.esp32c3", out.tag, out.tag_length);
    assert_span("SPI Flash Size : 4MB", out.message, out.message_length);
}

void test_empty_message() {
    IdfText out{};
    TEST_ASSERT_TRUE(parse("W (7) APP: \n", out));
    assert_span("APP", out.tag, out.tag_length);
    TEST_ASSERT_EQUAL_size_t(0, out.message_length);
}

void test_large_timestamp() {
    IdfText out{};
    TEST_ASSERT_TRUE(parse("I (4294967295) T: x\n", out));
}

// The hook formats into a fixed buffer: a long message may lose its final newline.
void test_truncated_text_without_newline() {
    IdfText out{};
    TEST_ASSERT_TRUE(parse("I (9) APP: a long message cut by the buff", out));
    assert_span("a long message cut by the buff", out.message, out.message_length);
}

// CONFIG_LOG_COLORS wraps the line in ANSI codes: "\033[0;32mI (..) TAG: msg\033[0m\n".
void test_ansi_color_prefix_and_reset_are_skipped() {
    IdfText out{};
    TEST_ASSERT_TRUE(parse("\x1b[0;31mE (1234) wifi: connection lost\x1b[0m\n", out));
    TEST_ASSERT_EQUAL_CHAR('E', logcat::level_to_char(out.level));
    assert_span("wifi", out.tag, out.tag_length);
    assert_span("connection lost", out.message, out.message_length);
}

void test_rejects_text_that_is_not_a_log_line() {
    const char* invalid[] = {
        "",
        "\n",
        "hello world\n",
        "X (1) T: m\n",            // unknown level
        "N (1) T: m\n",            // None is a threshold, never a line level
        "I 1) T: m\n",             // missing '('
        "I (1 T: m\n",             // missing ')'
        "I (1] T: m\n",            // wrong closing char, everything else right
        "I () T: m\n",             // no digits
        "I (12a) T: m\n",          // non-digit timestamp
        "I (1)T: m\n",             // missing space after ')'
        "I (1) T:m\n",             // missing space after ':'
        "I (1) : m\n",             // empty tag
        "I (1) no colon\n",        // no ": "
        "I (12345678901) T: m\n",  // timestamp over 10 digits
        "\x1b[0;31m",              // only an ANSI code
    };
    for (const char* text : invalid) {
        IdfText out{};
        TEST_ASSERT_FALSE_MESSAGE(parse(text, out), text);
    }
}

// The text is not NUL-terminated in general: nothing past `length` may be read.
void test_reads_only_within_length() {
    static char tail[64];
    const char line[] = "I (3) APP: msg\n";
    const size_t length = sizeof(line) - 1;
    for (size_t k = 0; k <= length; ++k) {
        char* start = tail + sizeof(tail) - k;
        std::memcpy(start, line, k);
        IdfText out{};
        logcat::parse_idf_text(start, k, out);
    }
    IdfText out{};
    char* start = tail + sizeof(tail) - length;
    TEST_ASSERT_TRUE(logcat::parse_idf_text(start, length, out));
    assert_span("msg", out.message, out.message_length);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parses_nominal_line);
    RUN_TEST(test_every_level_letter);
    RUN_TEST(test_tag_with_dots_and_message_with_colons);
    RUN_TEST(test_empty_message);
    RUN_TEST(test_large_timestamp);
    RUN_TEST(test_truncated_text_without_newline);
    RUN_TEST(test_ansi_color_prefix_and_reset_are_skipped);
    RUN_TEST(test_rejects_text_that_is_not_a_log_line);
    RUN_TEST(test_reads_only_within_length);
    return UNITY_END();
}
