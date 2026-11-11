#include "logcat/console.hpp"

#include <cstring>

namespace logcat {

bool LineAssembler::feed(char byte) {
    if (byte == '\n' || byte == '\r') {
        complete_ = pending_;
        pending_ = 0;
        return true;
    }
    if (pending_ < sizeof(buffer_)) {
        buffer_[pending_++] = byte;
    } else if (byte != ' ') {
        // Past the limit only one thing matters: whether the line was blank. Without this, 70
        // spaces and a command would look blank and get no reply instead of an error.
        buffer_[pending_ - 1] = byte;
    }
    return false;
}

ReplyFields::ReplyFields(Console& console, const char* id)
    : console_(console), id_(id), message_{}, data_(message_.line, id) {}

void ReplyFields::flush() {
    if (data_.fields() == 0) {
        return;
    }
    const size_t length = data_.finish();
    console_.send(ReplyKind::Data, id_, message_.line, length);
    data_ = ReplyData(message_.line, id_);
}

bool ReplyFields::add(const char* key, const char* value) {
    if (data_.add(key, value)) {
        return true;
    }
    if (data_.fields() == 0) {
        return false;  // an empty line refused it: the field itself is invalid
    }
    flush();
    return data_.add(key, value);
}

bool ReplyFields::add_uint(const char* key, uint32_t value) {
    if (data_.add_uint(key, value)) {
        return true;
    }
    if (data_.fields() == 0) {
        return false;
    }
    flush();
    return data_.add_uint(key, value);
}

void Console::feed(const char* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (assembler_.feed(bytes[i])) {
            execute(assembler_.line(), assembler_.length());
        }
    }
}

void Console::execute(const char* line, size_t length) {
    Command command;
    const CommandStatus status = parse_command(line, length, command);
    if (status == CommandStatus::Empty) {
        return;
    }
    send_failed_ = false;
    switch (status) {
        case CommandStatus::Ok:
            run(command);
            return;
        case CommandStatus::BadId:
            reply_error(command.id, error_code(status), "the first word is the id: 1-9999 or -");
            return;
        case CommandStatus::BadCommand:
            reply_error(command.id, error_code(status), "try: info stats log reboot crash");
            return;
        default:
            reply_error(command.id, error_code(status), "wrong arguments");
            return;
    }
}

void Console::run(const Command& command) {
    switch (command.kind) {
        case CommandKind::Info:
            reply_fields(command.id, &ConsolePlatform::fill_info);
            return;
        case CommandKind::Stats:
            reply_fields(command.id, &ConsolePlatform::fill_stats);
            return;
        case CommandKind::LogLevel:
            if (!filter_.set(command.tag, command.level)) {
                reply_error(command.id, "full", "tag table full (16 tags)");
                return;
            }
            platform_.set_log_level(command.tag, command.level);
            reply_ok(command.id);
            return;
        case CommandKind::LogTail:
            send_tail(command.id);
            return;
        case CommandKind::LogDump:
        case CommandKind::LogClear:
            reply_error(command.id, "unsupported", "needs flash storage (M4)");
            return;
        case CommandKind::Reboot:
            reply_ok(command.id);
            platform_.reboot();
            return;
        case CommandKind::CrashPanic:
            platform_.crash_panic();
            return;
        case CommandKind::CrashWdt:
            platform_.crash_wdt();
            return;
    }
}

void Console::reply_fields(const char* id, void (ConsolePlatform::*fill)(ReplyFields&)) {
    ReplyFields fields(*this, id);
    (platform_.*fill)(fields);
    fields.flush();
    reply_ok(id);
}

void Console::reply_ok(const char* id) {
    char line[kLineBufferSize];
    const size_t length = format_reply_ok(id, line);
    if (send_failed_) {
        reply_error(id, "timeout", "reply queue full");
        return;
    }
    send(ReplyKind::End, id, line, length);
}

void Console::reply_error(const char* id, const char* code, const char* text) {
    char line[kLineBufferSize];
    const size_t length = format_reply_error(id, code, text, line);
    send_failed_ = false;  // the terminator always gets its own try
    send(ReplyKind::End, id, line, length);
}

void Console::send_tail(const char* id) {
    send(ReplyKind::Tail, id, "", 0);
}

bool Console::send(ReplyKind kind, const char* id, const char* line, size_t length) {
    if (send_failed_) {
        return false;
    }
    ReplyMessage message{};
    message.kind = kind;
    message.length = static_cast<uint8_t>(length);
    std::memcpy(message.id, id, std::strlen(id) + 1);
    std::memcpy(message.line, line, length);
    send_failed_ = !platform_.send(message);
    return !send_failed_;
}

}  // namespace logcat
