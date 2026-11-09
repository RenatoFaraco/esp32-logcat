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

namespace {

std::vector<std::string> split(const std::string& row, char separator) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t end = row.find(separator, start);
        out.push_back(row.substr(start, end - start));
        if (end == std::string::npos) {
            return out;
        }
        start = end + 1;
    }
}

bool is_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    for (char c : hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::vector<char> from_hex(const std::string& hex) {
    std::vector<char> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16)));
    }
    return out;
}

struct CaseFile {
    std::vector<std::vector<std::string>> rows;
    long declared = -1;  // "# cases: N"
};

// `pio test` runs from the PlatformIO project directory (firmware/), in CI too.
CaseFile load_cases() {
    const char* candidates[] = {"../conformance/line_cases.tsv", "conformance/line_cases.tsv"};
    FILE* file = nullptr;
    for (const char* path : candidates) {
        file = std::fopen(path, "rb");
        if (file != nullptr) {
            break;
        }
    }
    CaseFile cases;
    if (file == nullptr) {
        return cases;
    }
    std::string row;
    int c = 0;
    while ((c = std::fgetc(file)) != EOF) {
        if (c != '\n') {
            row.push_back(static_cast<char>(c));
            continue;
        }
        if (row.rfind("# cases: ", 0) == 0) {
            cases.declared = std::strtol(row.c_str() + 9, nullptr, 10);
        } else if (!row.empty() && row[0] != '#') {
            cases.rows.push_back(split(row, '\t'));
        }
        row.clear();
    }
    std::fclose(file);
    return cases;
}

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
    if (parsed.boot != std::strtoul(f[3].c_str(), nullptr, 10)) why += " boot";
    if (parsed.seq != std::strtoul(f[4].c_str(), nullptr, 10)) why += " seq";
    if (parsed.uptime_ms != std::strtoul(f[5].c_str(), nullptr, 10)) why += " uptime";
    if (logcat::level_to_char(parsed.level) != f[6][0]) why += " level";
    if (f[7] != parsed.tag) why += " tag";
    const std::vector<char> msg = from_hex(f[8]);
    if (std::string(msg.begin(), msg.end()) != parsed.msg) why += " msg";
    if (parsed.truncated != (f[9] == "1")) why += " truncated";
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
    const CaseFile cases = load_cases();
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
