// The reply formatter against the shared reply cases (conformance/reply_cases.tsv, the same
// file the host parser checks): for every well-formed case, building the line from its fields
// gives the line byte for byte, and the block_crc cases check crc32_update. The "raw" cases
// are malformed lines for the host parser; here the unit tests below check that the formatter
// refuses to build each kind of malformed line.

#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "logcat/line_formatter.hpp"
#include "logcat/reply_formatter.hpp"

#include "../support/case_file.hpp"

using logcat::kLineBufferSize;
using logcat::kMaxLineLength;
using logcat::ReplyData;

namespace {

std::string text_of(const std::vector<char>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

// Lowercases the hex digits that end a "#END <id> <crc32>" line, not the marker.
std::string with_lower_crc(std::string line) {
    for (size_t i = line.size() >= 9 ? line.size() - 9 : 0; i < line.size(); ++i) {
        if (line[i] >= 'A' && line[i] <= 'F') {
            line[i] = static_cast<char>(line[i] - 'A' + 'a');
        }
    }
    return line;
}

std::string hex8(uint32_t value) {
    char out[9];
    std::snprintf(out, sizeof(out), "%08lx", static_cast<unsigned long>(value));
    return out;
}

// Builds the line of one case; "" when the formatter refused.
std::string build(const std::vector<std::string>& f) {
    char line[kLineBufferSize];
    std::memset(line, 0x5A, sizeof(line));
    const std::string& kind = f[1];
    const std::string& id = f[3];
    size_t length = 0;
    if (kind == "data") {
        ReplyData data(line, id.c_str());
        for (const std::string& field : test_support::split(f[4], ' ')) {
            const size_t equals = field.find('=');
            if (!data.add(field.substr(0, equals).c_str(), field.substr(equals + 1).c_str())) {
                return "";
            }
        }
        length = data.finish();
    } else if (kind == "end" && f[4] == "ok") {
        length = logcat::format_reply_ok(id.c_str(), line);
    } else if (kind == "end") {
        const std::string text = text_of(test_support::from_hex(f[5]));
        length = logcat::format_reply_error(id.c_str(), f[4].c_str(), text.c_str(), line);
    } else if (kind == "dump_begin") {
        const uint32_t bytes = static_cast<uint32_t>(std::strtoul(f[5].c_str(), nullptr, 10));
        length = logcat::format_dump_begin(id.c_str(), f[4].c_str(), bytes, line);
    } else if (kind == "dump_end") {
        const uint32_t crc = static_cast<uint32_t>(std::strtoul(f[4].c_str(), nullptr, 16));
        length = logcat::format_dump_end(id.c_str(), crc, line);
    }
    if (length == 0 || line[length] != '\0') {
        return "";
    }
    return std::string(line, length);
}

std::string check_case(const std::vector<std::string>& f) {
    if (f.size() < 3 || !test_support::is_hex(f[2])) {
        return f[0] + ": malformed row";
    }
    const std::string& name = f[0];
    const std::string& kind = f[1];
    const std::vector<char> bytes = test_support::from_hex(f[2]);
    if (kind == "raw") {
        return "";
    }
    if (kind == "block_crc") {
        const uint32_t whole = logcat::crc32_update(0, bytes.data(), bytes.size());
        // The same content fed in two pieces: the chaining the dump uses.
        const size_t half = bytes.size() / 2;
        uint32_t pieces = logcat::crc32_update(0, bytes.data(), half);
        pieces = logcat::crc32_update(pieces, bytes.data() + half, bytes.size() - half);
        if (hex8(whole) != f[3] || pieces != whole) {
            return name + ": crc32 " + hex8(whole) + ", expected " + f[3];
        }
        return "";
    }
    // The firmware always sends the CRC in lowercase; the host also accepts uppercase.
    const std::string expected =
        kind == "dump_end" ? with_lower_crc(text_of(bytes)) : text_of(bytes);
    const std::string built = build(f);
    if (built != expected) {
        return name + ": built \"" + built + "\"";
    }
    return "";
}

template <size_t N>
void assert_line(const char (&expected)[N], const char* line, size_t length) {
    TEST_ASSERT_EQUAL_size_t(N - 1, length);
    TEST_ASSERT_EQUAL_STRING(expected, line);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_formatter_reproduces_the_shared_reply_cases() {
    const test_support::CaseFile cases = test_support::load_cases("reply_cases.tsv");
    TEST_ASSERT_TRUE_MESSAGE(!cases.rows.empty(),
                             "conformance/reply_cases.tsv not found (run from firmware/)");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(static_cast<size_t>(cases.declared), cases.rows.size(),
                                     "case count differs from the '# cases:' header");
    std::string failures;
    size_t failed = 0;
    size_t built = 0;
    for (const std::vector<std::string>& row : cases.rows) {
        const std::string problem = check_case(row);
        built += row[1] != "raw" ? 1 : 0;
        if (!problem.empty()) {
            failures += problem + "\n";
            ++failed;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(built >= 30, "too few well-formed cases: did the file change?");
    if (failed != 0) {
        std::printf("%s", failures.c_str());
        TEST_FAIL_MESSAGE("shared reply cases disagree with the formatter (listed above)");
    }
}

// ---------------------------------------------------------------- ids

void test_reply_ids() {
    for (const char* id : {"-", "1", "7", "42", "999", "9999"}) {
        TEST_ASSERT_TRUE_MESSAGE(logcat::is_reply_id(id), id);
    }
    for (const char* id : {"", "0", "07", "10000", "--", "-1", "1a", "a", " 1", "1 ", "+1"}) {
        TEST_ASSERT_FALSE_MESSAGE(logcat::is_reply_id(id), id);
    }
    TEST_ASSERT_FALSE(logcat::is_reply_id(nullptr));
}

void test_every_line_kind_refuses_a_bad_id() {
    char line[kLineBufferSize];
    for (const char* id : {"0", "07", "10000", "", "x"}) {
        ReplyData data(line, id);
        TEST_ASSERT_FALSE(data.add("a", "1"));
        TEST_ASSERT_EQUAL_size_t(0, data.finish());
        TEST_ASSERT_EQUAL_size_t(0, logcat::format_reply_ok(id, line));
        TEST_ASSERT_EQUAL_size_t(0, logcat::format_reply_error(id, "full", nullptr, line));
        TEST_ASSERT_EQUAL_size_t(0, logcat::format_dump_begin(id, "ram", 1, line));
        TEST_ASSERT_EQUAL_size_t(0, logcat::format_dump_end(id, 1, line));
    }
}

// ---------------------------------------------------------------- data lines

void test_data_line_with_text_and_numbers() {
    char line[kLineBufferSize];
    ReplyData data(line, "7");
    TEST_ASSERT_TRUE(data.add("fw", "0.3.0"));
    TEST_ASSERT_TRUE(data.add_uint("proto", 1));
    TEST_ASSERT_TRUE(data.add_uint("uptime_ms", 4294967295u));
    TEST_ASSERT_TRUE(data.add_uint("not_sent.usb", 0));
    TEST_ASSERT_EQUAL_size_t(4, data.fields());
    const size_t length = data.finish();
    assert_line("#> 7 fw=0.3.0 proto=1 uptime_ms=4294967295 not_sent.usb=0\n", line, length);
}

void test_data_line_without_fields_is_not_a_line() {
    char line[kLineBufferSize];
    ReplyData data(line, "7");
    TEST_ASSERT_EQUAL_size_t(0, data.finish());
    TEST_ASSERT_EQUAL_STRING("", line);
}

void test_bad_keys_are_refused_and_leave_the_line_intact() {
    char line[kLineBufferSize];
    ReplyData data(line, "7");
    TEST_ASSERT_TRUE(data.add("a", "1"));
    const std::string key33(33, 'k');
    for (const char* key : {"", "Fw", "1a", ".a", "_a", "a b", "a=b", "a-b", key33.c_str()}) {
        TEST_ASSERT_FALSE_MESSAGE(data.add(key, "1"), key);
    }
    TEST_ASSERT_FALSE(data.add(nullptr, "1"));
    const std::string key32(32, 'k');
    TEST_ASSERT_TRUE(data.add(key32.c_str(), "1"));
    const std::string expected = "#> 7 a=1 " + key32 + "=1\n";
    const size_t length = data.finish();
    TEST_ASSERT_EQUAL_size_t(expected.size(), length);
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), line);
}

void test_bad_values_are_refused() {
    char line[kLineBufferSize];
    ReplyData data(line, "7");
    for (const char* value : {"", "a b", " a", "a ", "a\tb", "a\nb", "\x7f", "caf\xc3\xa9"}) {
        TEST_ASSERT_FALSE_MESSAGE(data.add("k", value), value);
    }
    TEST_ASSERT_FALSE(data.add("k", nullptr));
    TEST_ASSERT_TRUE(data.add("k", "a=b/c~!"));
    assert_line("#> 7 k=a=b/c~!\n", line, data.finish());
}

void test_a_key_appears_once_per_line() {
    char line[kLineBufferSize];
    ReplyData data(line, "7");
    TEST_ASSERT_TRUE(data.add("ab", "1"));
    TEST_ASSERT_TRUE(data.add("a", "x=ab=2"));  // "ab=" inside a value is not the key ab
    TEST_ASSERT_TRUE(data.add("abc", "3"));     // a longer key with the same prefix
    TEST_ASSERT_FALSE(data.add("ab", "4"));
    TEST_ASSERT_FALSE(data.add("a", "5"));
    TEST_ASSERT_FALSE(data.add("abc", "6"));
    TEST_ASSERT_TRUE(data.add("b", "7"));  // "b" is the end of "ab", not a key of its own
    assert_line("#> 7 ab=1 a=x=ab=2 abc=3 b=7\n", line, data.finish());
}

void test_a_field_that_does_not_fit_is_refused_and_the_line_still_closes() {
    char line[kLineBufferSize];
    ReplyData data(line, "9999");
    // "#> 9999 a=" + 159 chars + '\n' = 170 bytes: the limit.
    const std::string fits(159, 'x');
    const std::string too_long(160, 'x');
    TEST_ASSERT_FALSE(data.add("a", too_long.c_str()));
    TEST_ASSERT_TRUE(data.add("a", fits.c_str()));
    TEST_ASSERT_FALSE(data.add("b", "1"));  // no room for " b=1"
    const size_t length = data.finish();
    TEST_ASSERT_EQUAL_size_t(kMaxLineLength, length);
    TEST_ASSERT_EQUAL_CHAR('\n', line[length - 1]);
    TEST_ASSERT_EQUAL_CHAR('\0', line[length]);
}

void test_the_last_byte_of_room_is_usable() {
    char line[kLineBufferSize];
    ReplyData data(line, "1");
    // "#> 1 a=" + 158 chars = 165 bytes; " b=1" fills the body to exactly 169.
    TEST_ASSERT_TRUE(data.add("a", std::string(158, 'x').c_str()));
    TEST_ASSERT_FALSE(data.add("b", "12"));
    TEST_ASSERT_TRUE(data.add("b", "1"));
    TEST_ASSERT_FALSE(data.add("c", "1"));
    TEST_ASSERT_EQUAL_size_t(kMaxLineLength, data.finish());
}

// ---------------------------------------------------------------- terminators

void test_ok_and_error_lines() {
    char line[kLineBufferSize];
    assert_line("#. 7 ok\n", line, logcat::format_reply_ok("7", line));
    assert_line("#. - ok\n", line, logcat::format_reply_ok("-", line));
    assert_line("#! 3 bad_args\n", line,
                logcat::format_reply_error("3", "bad_args", nullptr, line));
    assert_line("#! 3 bad_args\n", line, logcat::format_reply_error("3", "bad_args", "", line));
    assert_line("#! 3 full tag table full (16 tags)\n", line,
                logcat::format_reply_error("3", "full", "tag table full (16 tags)", line));
}

void test_bad_error_codes_are_refused() {
    char line[kLineBufferSize];
    const std::string code33(33, 'e');
    for (const char* code :
         {"", "ok", "FULL", "Full", "full1", "bad-args", "bad args", code33.c_str()}) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, logcat::format_reply_error("3", code, "x", line), code);
        TEST_ASSERT_EQUAL_STRING("", line);
    }
    TEST_ASSERT_EQUAL_size_t(0, logcat::format_reply_error("3", nullptr, "x", line));
    const std::string code32(32, 'e');
    TEST_ASSERT_EQUAL_size_t(5 + 32 + 1, logcat::format_reply_error("3", code32.c_str(), "", line));
    assert_line("#! 3 okay\n", line, logcat::format_reply_error("3", "okay", nullptr, line));
}

void test_bad_error_texts_are_refused() {
    char line[kLineBufferSize];
    for (const char* text :
         {" leading space", "tab\there", "line\nbreak", "esc\x1b[2J", "caf\xc3\xa9", "del\x7f"}) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, logcat::format_reply_error("3", "failed", text, line),
                                         text);
        TEST_ASSERT_EQUAL_STRING("", line);
    }
    assert_line(
        "#! 3 failed a  b \n", line,
        logcat::format_reply_error("3", "failed", "a  b ", line));  // spaces inside are fine
}

void test_error_text_is_cut_at_the_line_limit() {
    char line[kLineBufferSize];
    const std::string text(400, 'y');
    const size_t length = logcat::format_reply_error("1", "failed", text.c_str(), line);
    TEST_ASSERT_EQUAL_size_t(kMaxLineLength, length);
    const std::string expected = "#! 1 failed " + std::string(170 - 12 - 1, 'y') + "\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), line);
}

// ---------------------------------------------------------------- dump block markers

void test_dump_markers() {
    char line[kLineBufferSize];
    assert_line("#BEGIN 4 log_003.txt 4096\n", line,
                logcat::format_dump_begin("4", "log_003.txt", 4096, line));
    assert_line("#BEGIN - ram 0\n", line, logcat::format_dump_begin("-", "ram", 0, line));
    assert_line("#BEGIN 4 f 4294967295\n", line,
                logcat::format_dump_begin("4", "f", 4294967295u, line));
    assert_line("#END 4 1a2b3c4d\n", line, logcat::format_dump_end("4", 0x1A2B3C4Du, line));
    assert_line("#END - 00000000\n", line, logcat::format_dump_end("-", 0, line));
    assert_line("#END 4 ffffffff\n", line, logcat::format_dump_end("4", 0xFFFFFFFFu, line));
}

void test_bad_dump_names_are_refused() {
    char line[kLineBufferSize];
    const std::string name65(65, 'n');
    for (const char* name :
         {"", "../etc", "-f", ".hidden", "a/b", "a b", "a\\b", "caf\xc3\xa9", name65.c_str()}) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, logcat::format_dump_begin("4", name, 1, line), name);
        TEST_ASSERT_EQUAL_STRING("", line);
    }
    TEST_ASSERT_EQUAL_size_t(0, logcat::format_dump_begin("4", nullptr, 1, line));
    const std::string name64(64, 'n');
    TEST_ASSERT_TRUE(logcat::format_dump_begin("4", name64.c_str(), 1, line) != 0);
    TEST_ASSERT_TRUE(logcat::format_dump_begin("4", "_a..b-c", 1, line) != 0);
}

// ---------------------------------------------------------------- CRC-32

void test_crc32_known_values() {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, logcat::crc32_update(0, "123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, logcat::crc32_update(0, "", 0));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, logcat::crc32_update(0, nullptr, 0));
    TEST_ASSERT_EQUAL_HEX32(0xD202EF8Du, logcat::crc32_update(0, "\0", 1));
    TEST_ASSERT_EQUAL_HEX32(0xFF000000u, logcat::crc32_update(0, "\xff", 1));
}

void test_crc32_byte_by_byte_equals_one_call() {
    unsigned char content[300];
    for (size_t i = 0; i < sizeof(content); ++i) {
        content[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    uint32_t crc = 0;
    for (const unsigned char byte : content) {
        crc = logcat::crc32_update(crc, &byte, 1);
    }
    TEST_ASSERT_EQUAL_HEX32(logcat::crc32_update(0, content, sizeof(content)), crc);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_formatter_reproduces_the_shared_reply_cases);
    RUN_TEST(test_reply_ids);
    RUN_TEST(test_every_line_kind_refuses_a_bad_id);
    RUN_TEST(test_data_line_with_text_and_numbers);
    RUN_TEST(test_data_line_without_fields_is_not_a_line);
    RUN_TEST(test_bad_keys_are_refused_and_leave_the_line_intact);
    RUN_TEST(test_bad_values_are_refused);
    RUN_TEST(test_a_key_appears_once_per_line);
    RUN_TEST(test_a_field_that_does_not_fit_is_refused_and_the_line_still_closes);
    RUN_TEST(test_the_last_byte_of_room_is_usable);
    RUN_TEST(test_ok_and_error_lines);
    RUN_TEST(test_bad_error_codes_are_refused);
    RUN_TEST(test_bad_error_texts_are_refused);
    RUN_TEST(test_error_text_is_cut_at_the_line_limit);
    RUN_TEST(test_dump_markers);
    RUN_TEST(test_bad_dump_names_are_refused);
    RUN_TEST(test_crc32_known_values);
    RUN_TEST(test_crc32_byte_by_byte_equals_one_call);
    return UNITY_END();
}
