// The shared line-format cases (conformance/line_cases.tsv), also read by the Python host suite
// (host/tests/test_conformance.py). One file, two parsers: they cannot drift apart (#20).
//
// For every case: parse_line gives the expected status and fields, and leaves `out` untouched
// when it rejects the line. For valid lines also format_line(parsed) == line, so the valid lines
// of the file are exactly what the C++ formatter writes (the cross-language round trip).
//
// Every case is checked and every mismatch is reported (not only the first one), and each line
// is parsed from a buffer of its exact size so ASan (native_asan) sees any read past the end.

#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "logcat/line_formatter.hpp"
#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"

#include "../support/case_file.hpp"

using test_support::CaseFile;
using test_support::from_hex;
using test_support::is_hex;

namespace {

std::string check_case(const std::vector<std::string>& f) {
    const std::string& name = f[0];
    const std::string& status = f[1];
    const bool ok = status == "ok";
    if (f.size() != (ok ? 10u : 3u) || !is_hex(f[2])) {
        return name + ": malformed row";
    }
    const std::vector<char> line = from_hex(f[2]);  // exact size: no slack for ASan to miss
    logcat::LogRecord parsed{};
    std::memset(&parsed, 0xA5, sizeof(parsed));
    logcat::LogRecord untouched{};
    std::memcpy(&untouched, &parsed, sizeof(parsed));

    const logcat::ParseStatus result = logcat::parse_line(line.data(), line.size(), parsed);
    if (status != logcat::to_string(result)) {
        return name + ": expected " + status + ", got " + logcat::to_string(result);
    }
    if (!ok) {
        if (std::memcmp(&parsed, &untouched, sizeof(parsed)) != 0) {
            return name + ": out was written for a rejected line";
        }
        return "";
    }
    std::string why;
    if (parsed.boot != std::strtoul(f[3].c_str(), nullptr, 10))
        why += " boot";
    if (parsed.seq != std::strtoul(f[4].c_str(), nullptr, 10))
        why += " seq";
    if (parsed.uptime_ms != std::strtoul(f[5].c_str(), nullptr, 10))
        why += " uptime";
    if (logcat::level_to_char(parsed.level) != f[6][0])
        why += " level";
    if (f[7] != parsed.tag)
        why += " tag";
    const std::vector<char> msg = from_hex(f[8]);
    if (std::string(msg.begin(), msg.end()) != parsed.msg)
        why += " msg";
    if (parsed.truncated != (f[9] == "1"))
        why += " truncated";
    char formatted[logcat::kLineBufferSize];
    const size_t length = logcat::format_line(parsed, formatted);
    if (length != line.size() || std::memcmp(formatted, line.data(), length) != 0) {
        why += " format_line";
    }
    return why.empty() ? "" : name + ": wrong" + why;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_cpp_parser_and_formatter_agree_with_the_shared_cases() {
    const CaseFile cases = test_support::load_cases("line_cases.tsv");
    TEST_ASSERT_TRUE_MESSAGE(!cases.rows.empty(),
                             "conformance/line_cases.tsv not found or empty (run from firmware/)");
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
        TEST_FAIL_MESSAGE("shared cases disagree with the C++ parser (listed above)");
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_cpp_parser_and_formatter_agree_with_the_shared_cases);
    return UNITY_END();
}
