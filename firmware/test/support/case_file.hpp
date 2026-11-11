// Reader of the shared case files in conformance/*.tsv, for the native test suites.
// Test-only code: std::string and std::vector are fine here (never in the core).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace test_support {

inline std::vector<std::string> split(const std::string& row, char separator) {
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

inline bool is_hex(const std::string& hex) {
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

// Exact size: a line parsed from this buffer has no slack for ASan to miss a read past the end.
inline std::vector<char> from_hex(const std::string& hex) {
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

// `name` is a file of conformance/. `pio test` runs from the PlatformIO project directory
// (firmware/), in CI too.
inline CaseFile load_cases(const std::string& name) {
    const std::string candidates[] = {"../conformance/" + name, "conformance/" + name};
    FILE* file = nullptr;
    for (const std::string& path : candidates) {
        file = std::fopen(path.c_str(), "rb");
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

}  // namespace test_support
