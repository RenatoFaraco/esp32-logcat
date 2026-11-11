#pragma once

#include <cstddef>
#include <cstdint>

#include "logcat/command_parser.hpp"
#include "logcat/reply_formatter.hpp"
#include "logcat/reply_server.hpp"
#include "logcat/tag_level_filter.hpp"

namespace logcat {

// Splits the bytes received into request lines, at '\n' or '\r'. Keeps at most
// kMaxRequestLength + 1 bytes of a line and discards the rest up to the line ending, so the
// parser rejects an overlong line whole and its tail is never taken as a new request.
class LineAssembler {
public:
    // True when `byte` completed a line: read it with line()/length() before the next feed().
    bool feed(char byte);

    const char* line() const { return buffer_; }
    size_t length() const { return complete_; }

private:
    char buffer_[kMaxRequestLength + 1] = {};
    size_t pending_ = 0;
    size_t complete_ = 0;
};

class Console;

// The fields of a reply's data lines. A field that does not fit on the current line goes on the
// next one, so `stats` can grow without anyone counting bytes. A field the firmware cannot
// measure is simply not added (ADR-012 §2).
class ReplyFields {
public:
    bool add(const char* key, const char* value);
    bool add_uint(const char* key, uint32_t value);

private:
    friend class Console;
    ReplyFields(Console& console, const char* id);
    void flush();

    Console& console_;
    const char* id_;
    ReplyMessage message_;
    ReplyData data_;
};

// What the console needs from the platform (ports & adapters, ADR-003).
class ConsolePlatform {
public:
    // Queues one message for the writer task, waiting for room with a timeout. false = timeout.
    virtual bool send(const ReplyMessage& message) = 0;
    virtual void fill_info(ReplyFields& fields) = 0;
    virtual void fill_stats(ReplyFields& fields) = 0;
    // The platform's own gate, when it has one (ESP-IDF drops a line before the hook sees it).
    virtual void set_log_level(const char* tag, LogLevel level) = 0;
    // Called after the reply was queued. These do not return on the board.
    virtual void reboot() = 0;
    virtual void crash_panic() = 0;
    virtual void crash_wdt() = 0;

protected:
    ~ConsolePlatform() = default;
};

// The console's logic: bytes in, commands executed, replies out (wire-format §2). Owned by one
// task, and that task is the only writer of `filter`, as TagLevelFilter requires. No heap.
class Console {
public:
    Console(ConsolePlatform& platform, TagLevelFilter& filter)
        : platform_(platform), filter_(filter) {}

    // Feeds received bytes; executes every line they complete.
    void feed(const char* bytes, size_t length);

private:
    friend class ReplyFields;

    void execute(const char* line, size_t length);
    void run(const Command& command);
    void reply_fields(const char* id, void (ConsolePlatform::*fill)(ReplyFields&));
    void reply_ok(const char* id);
    void reply_error(const char* id, const char* code, const char* text);
    void send_tail(const char* id);
    bool send(ReplyKind kind, const char* id, const char* line, size_t length);

    ConsolePlatform& platform_;
    TagLevelFilter& filter_;
    LineAssembler assembler_;
    bool send_failed_ = false;  // a line of the current reply found no room in the queue
};

}  // namespace logcat
