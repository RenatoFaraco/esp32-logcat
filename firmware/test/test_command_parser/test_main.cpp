// parse_command against the shared request cases (conformance/request_cases.tsv, also the
// contract for the Rust firmware of ADR-011), plus what a table of lines cannot express: what
// is left untouched on rejection, and inputs that are not C strings.
//
// Every case is checked and every mismatch is reported, and each line is parsed from a buffer
// of its exact size so ASan (native_asan) sees any read past the end.

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "logcat/command_parser.hpp"
#include "logcat/log_record.hpp"

#include "../support/case_file.hpp"

using logcat::Command;
using logcat::CommandKind;
using logcat::CommandStatus;
using logcat::LogLevel;
using logcat::parse_command;

namespace {

constexpr unsigned char kFill = 0xA5;

Command filled() {
    Command command;
    std::memset(&command, kFill, sizeof(command));
    return command;
}

bool same_bytes(const void* a, const void* b, size_t size) {
    return std::memcmp(a, b, size) == 0;
}

CommandStatus parse(const std::string& line, Command& out) {
    const std::vector<char> exact(line.begin(), line.end());
    return parse_command(exact.data(), exact.size(), out);
}

// 'N' for None: the letter the command uses, not the '?' of level_to_char.
char level_letter(LogLevel level) {
    return level == LogLevel::None ? 'N' : logcat::level_to_char(level);
}

std::string check_case(const std::vector<std::string>& f) {
    if (f.size() != 7 || !test_support::is_hex(f[2])) {
        return f[0] + ": malformed row";
    }
    const std::string& name = f[0];
    const std::string& status = f[1];
    const std::vector<char> line = test_support::from_hex(f[2]);
    Command parsed = filled();
    const Command untouched = filled();

    const CommandStatus result = parse_command(line.data(), line.size(), parsed);
    if (status != logcat::to_string(result)) {
        return name + ": expected " + status + ", got " + logcat::to_string(result);
    }
    if (result == CommandStatus::Empty) {
        return same_bytes(&parsed, &untouched, sizeof(parsed)) ? ""
                                                               : name + ": out written for empty";
    }
    if (std::memchr(parsed.id, '\0', sizeof(parsed.id)) == nullptr || f[3] != parsed.id) {
        return name + ": wrong id";
    }
    if (result != CommandStatus::Ok) {
        const bool kept = same_bytes(&parsed.kind, &untouched.kind, sizeof(parsed.kind)) &&
                          same_bytes(parsed.tag, untouched.tag, sizeof(parsed.tag)) &&
                          same_bytes(&parsed.level, &untouched.level, sizeof(parsed.level));
        return kept ? "" : name + ": kind, tag or level written for a rejected request";
    }
    std::string why;
    if (f[4] != logcat::to_string(parsed.kind)) {
        why += " kind";
    }
    if (std::memchr(parsed.tag, '\0', sizeof(parsed.tag)) == nullptr || f[5] != parsed.tag) {
        why += " tag";
    }
    const char expected_level = f[6].empty() ? 'N' : f[6][0];
    if (level_letter(parsed.level) != expected_level) {
        why += " level";
    }
    return why.empty() ? "" : name + ": wrong" + why;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_parser_agrees_with_the_shared_request_cases() {
    const test_support::CaseFile cases = test_support::load_cases("request_cases.tsv");
    TEST_ASSERT_TRUE_MESSAGE(!cases.rows.empty(),
                             "conformance/request_cases.tsv not found (run from firmware/)");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(static_cast<size_t>(cases.declared), cases.rows.size(),
                                     "case count differs from the '# cases:' header");
    std::string failures;
    size_t failed = 0;
    for (const std::vector<std::string>& row : cases.rows) {
        const std::string problem = check_case(row);
        if (!problem.empty()) {
            failures += problem + "\n";
            ++failed;
        }
    }
    if (failed != 0) {
        std::printf("%s", failures.c_str());
        TEST_FAIL_MESSAGE("shared request cases disagree with parse_command (listed above)");
    }
}

void test_null_line_with_zero_length_is_empty() {
    Command out = filled();
    TEST_ASSERT_EQUAL_STRING("empty", logcat::to_string(parse_command(nullptr, 0, out)));
}

void test_commands_without_arguments_clear_tag_and_level() {
    Command out = filled();
    TEST_ASSERT_EQUAL_STRING("ok", logcat::to_string(parse("7 info\n", out)));
    TEST_ASSERT_EQUAL_STRING("", out.tag);
    TEST_ASSERT_EQUAL_CHAR('N', level_letter(out.level));
}

void test_every_level_letter_maps_to_its_level() {
    const struct {
        char letter;
        LogLevel level;
    } expected[] = {{'E', LogLevel::Error}, {'W', LogLevel::Warn},    {'I', LogLevel::Info},
                    {'D', LogLevel::Debug}, {'V', LogLevel::Verbose}, {'N', LogLevel::None}};
    for (const auto& item : expected) {
        Command out = filled();
        const std::string line = std::string("1 log level APP ") + item.letter + "\n";
        TEST_ASSERT_EQUAL_STRING("ok", logcat::to_string(parse(line, out)));
        TEST_ASSERT_TRUE_MESSAGE(out.level == item.level, line.c_str());
    }
}

void test_every_byte_but_printable_ascii_is_rejected_anywhere() {
    const std::string base = "7 log level APP D";
    for (size_t position = 0; position <= base.size(); ++position) {
        for (int byte = 0; byte < 256; ++byte) {
            if (byte >= 0x20 && byte <= 0x7E) {
                continue;
            }
            std::string line = base;
            line.insert(position, 1, static_cast<char>(byte));
            if (position == base.size() && (byte == '\n' || byte == '\r')) {
                continue;  // that is a line ending
            }
            Command out = filled();
            TEST_ASSERT_EQUAL_STRING("bad_args", logcat::to_string(parse(line, out)));
            TEST_ASSERT_EQUAL_STRING("-", out.id);
        }
    }
}

void test_error_codes_of_the_reply() {
    TEST_ASSERT_NULL(logcat::error_code(CommandStatus::Ok));
    TEST_ASSERT_NULL(logcat::error_code(CommandStatus::Empty));
    TEST_ASSERT_EQUAL_STRING("bad_args", logcat::error_code(CommandStatus::BadId));
    TEST_ASSERT_EQUAL_STRING("bad_command", logcat::error_code(CommandStatus::BadCommand));
    TEST_ASSERT_EQUAL_STRING("bad_args", logcat::error_code(CommandStatus::BadArgs));
}

void test_names_of_kinds_are_distinct() {
    const CommandKind kinds[] = {
        CommandKind::Info,    CommandKind::Stats,      CommandKind::LogDump,
        CommandKind::LogTail, CommandKind::LogClear,   CommandKind::LogLevel,
        CommandKind::Reboot,  CommandKind::CrashPanic, CommandKind::CrashWdt};
    for (const CommandKind a : kinds) {
        for (const CommandKind b : kinds) {
            if (a != b) {
                TEST_ASSERT_TRUE(std::strcmp(logcat::to_string(a), logcat::to_string(b)) != 0);
            }
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parser_agrees_with_the_shared_request_cases);
    RUN_TEST(test_null_line_with_zero_length_is_empty);
    RUN_TEST(test_commands_without_arguments_clear_tag_and_level);
    RUN_TEST(test_every_level_letter_maps_to_its_level);
    RUN_TEST(test_every_byte_but_printable_ascii_is_rejected_anywhere);
    RUN_TEST(test_error_codes_of_the_reply);
    RUN_TEST(test_names_of_kinds_are_distinct);
    return UNITY_END();
}
