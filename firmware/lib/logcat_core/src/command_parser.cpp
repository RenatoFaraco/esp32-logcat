#include "logcat/command_parser.hpp"

#include <cstring>

namespace logcat {

namespace {

struct Word {
    const char* text;
    size_t length;
};

// The longest command has 5 words (`<id> log level <tag> <L>`); one more slot is enough to
// tell "too many" without storing them all.
constexpr size_t kMaxWords = 6;

constexpr bool is_printable(unsigned char byte) {
    return byte >= 0x20 && byte <= 0x7E;
}

// Splits at runs of spaces. Returns the number of words, capped at kMaxWords.
size_t split_words(const char* line, size_t length, Word (&words)[kMaxWords]) {
    size_t count = 0;
    size_t i = 0;
    while (i < length && count < kMaxWords) {
        while (i < length && line[i] == ' ') {
            ++i;
        }
        if (i == length) {
            break;
        }
        const size_t start = i;
        while (i < length && line[i] != ' ') {
            ++i;
        }
        words[count++] = Word{line + start, i - start};
    }
    return count;
}

template <size_t N>
bool equals(const Word& word, const char (&literal)[N]) {
    return word.length == N - 1 && std::memcmp(word.text, literal, N - 1) == 0;
}

// "-" or 1..9999 without a leading zero (wire-format §2.1).
bool is_id(const Word& word) {
    if (word.length == 1 && word.text[0] == '-') {
        return true;
    }
    if (word.length < 1 || word.length > kRequestIdCapacity - 1 || word.text[0] == '0') {
        return false;
    }
    for (size_t i = 0; i < word.length; ++i) {
        if (word.text[i] < '0' || word.text[i] > '9') {
            return false;
        }
    }
    return true;
}

// Copies at most capacity - 1 chars and terminates: a longer word is cut.
template <size_t N>
void copy_word(const Word& word, char (&out)[N]) {
    const size_t length = word.length < N - 1 ? word.length : N - 1;
    std::memcpy(out, word.text, length);
    out[length] = '\0';
}

// Its own mapping: level_from_char knows only the letters a line can carry, and the command
// also takes N (None: silence the tag).
bool threshold_from_word(const Word& word, LogLevel& level) {
    if (word.length != 1) {
        return false;
    }
    if (word.text[0] == 'N') {
        level = LogLevel::None;
        return true;
    }
    return level_from_char(word.text[0], level);
}

CommandStatus accept(Command& out, CommandKind kind) {
    out.kind = kind;
    out.tag[0] = '\0';
    out.level = LogLevel::None;
    return CommandStatus::Ok;
}

// `words` starts at the command, after the id.
CommandStatus parse_words(const Word* words, size_t count, Command& out) {
    if (count == 0) {
        return CommandStatus::BadCommand;
    }
    const Word& name = words[0];
    if (equals(name, "info") || equals(name, "stats") || equals(name, "reboot")) {
        if (count != 1) {
            return CommandStatus::BadArgs;
        }
        return accept(out, equals(name, "info")    ? CommandKind::Info
                           : equals(name, "stats") ? CommandKind::Stats
                                                   : CommandKind::Reboot);
    }
    if (equals(name, "crash")) {
        if (count != 2) {
            return CommandStatus::BadArgs;
        }
        if (equals(words[1], "panic")) {
            return accept(out, CommandKind::CrashPanic);
        }
        if (equals(words[1], "wdt")) {
            return accept(out, CommandKind::CrashWdt);
        }
        return CommandStatus::BadArgs;
    }
    if (!equals(name, "log")) {
        return CommandStatus::BadCommand;
    }
    // The second word is part of the command's name: missing = arguments missing, unknown =
    // unknown command.
    if (count == 1) {
        return CommandStatus::BadArgs;
    }
    const Word& sub = words[1];
    if (equals(sub, "dump") || equals(sub, "tail") || equals(sub, "clear")) {
        if (count != 2) {
            return CommandStatus::BadArgs;
        }
        return accept(out, equals(sub, "dump")   ? CommandKind::LogDump
                           : equals(sub, "tail") ? CommandKind::LogTail
                                                 : CommandKind::LogClear);
    }
    if (!equals(sub, "level")) {
        return CommandStatus::BadCommand;
    }
    LogLevel level = LogLevel::None;
    if (count != 4 || !threshold_from_word(words[3], level)) {
        return CommandStatus::BadArgs;
    }
    out.kind = CommandKind::LogLevel;
    copy_word(words[2], out.tag);
    out.level = level;
    return CommandStatus::Ok;
}

}  // namespace

const char* to_string(CommandKind kind) {
    switch (kind) {
        case CommandKind::Info:
            return "info";
        case CommandKind::Stats:
            return "stats";
        case CommandKind::LogDump:
            return "log_dump";
        case CommandKind::LogTail:
            return "log_tail";
        case CommandKind::LogClear:
            return "log_clear";
        case CommandKind::LogLevel:
            return "log_level";
        case CommandKind::Reboot:
            return "reboot";
        case CommandKind::CrashPanic:
            return "crash_panic";
        case CommandKind::CrashWdt:
            return "crash_wdt";
    }
    return "unknown";
}

const char* to_string(CommandStatus status) {
    switch (status) {
        case CommandStatus::Ok:
            return "ok";
        case CommandStatus::Empty:
            return "empty";
        case CommandStatus::BadId:
            return "bad_id";
        case CommandStatus::BadCommand:
            return "bad_command";
        case CommandStatus::BadArgs:
            return "bad_args";
    }
    return "unknown";
}

const char* error_code(CommandStatus status) {
    switch (status) {
        case CommandStatus::Ok:
        case CommandStatus::Empty:
            return nullptr;
        case CommandStatus::BadCommand:
            return "bad_command";
        case CommandStatus::BadId:
        case CommandStatus::BadArgs:
            return "bad_args";
    }
    return "bad_args";
}

CommandStatus parse_command(const char* line, size_t length, Command& out) {
    // One line ending, as terminals send it: "\n", "\r\n" or "\r". Anything left of it that is
    // not printable (a second "\n", a TAB, an escape sequence) rejects the line below.
    if (length > 0 && line[length - 1] == '\n') {
        --length;
    }
    if (length > 0 && line[length - 1] == '\r') {
        --length;
    }
    bool blank = true;
    bool printable = true;
    for (size_t i = 0; i < length; ++i) {
        blank = blank && line[i] == ' ';
        printable = printable && is_printable(static_cast<unsigned char>(line[i]));
    }
    if (blank) {
        return CommandStatus::Empty;
    }
    // A damaged or overlong line has no id worth trusting: the reply goes to "-".
    if (!printable || length > kMaxRequestLength) {
        std::memcpy(out.id, "-", 2);
        return CommandStatus::BadArgs;
    }
    Word words[kMaxWords] = {};
    const size_t count = split_words(line, length, words);
    if (!is_id(words[0])) {
        std::memcpy(out.id, "-", 2);
        return CommandStatus::BadId;
    }
    copy_word(words[0], out.id);
    return parse_words(words + 1, count - 1, out);
}

}  // namespace logcat
