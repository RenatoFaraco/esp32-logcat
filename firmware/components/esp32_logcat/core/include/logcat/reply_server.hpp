#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "logcat/command_parser.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/log_archive.hpp"
#include "logcat/ram_tail.hpp"
#include "logcat/sink.hpp"

namespace logcat {

enum class ReplyKind : uint8_t {
    Data,  // a data line: more lines of this reply follow
    End,   // the terminator: the last line of this reply
    Tail,  // no line: the writer task serves the RAM tail as a block and terminates the reply
    Dump,  // no line: the writer task serves the log files as blocks and terminates the reply
};

// Bytes of a log file read and sent per step of a dump. LittleFS opens and seeks on every
// read, so less would cost more opens (file_system.hpp); the USB driver's TX buffer (1 KB)
// takes it whole.
inline constexpr size_t kDumpChunk = 512;

// One entry of the reply queue, from the console task to the writer task. Copied by value
// through the FreeRTOS queue, like LogRecord.
struct ReplyMessage {
    ReplyKind kind;
    uint8_t length;  // bytes of `line`, '\n' included
    char id[kRequestIdCapacity];
    char line[kLineBufferSize];
};
static_assert(std::is_trivially_copyable_v<ReplyMessage>, "copied with memcpy");

// How a reply reaches the host. Unlike Sink::write, this may wait, for a short bounded time
// (ADR-012 §5): a reply is worth waiting for, a log line is not. Whole line or nothing.
class ReplyOutput {
public:
    virtual bool write_reply(const char* line, size_t length) = 0;
    // The same, without waiting: for the "#! timeout" sent after a write already timed out.
    virtual bool try_write_reply(const char* line, size_t length) = 0;

protected:
    ~ReplyOutput() = default;  // never deleted through the interface: outputs are statics
};

// Takes a sink out of the log stream while a dump block is open: a live line, identical in form
// to the block's content, would break its byte count and CRC. The Writer counts the refused
// lines and reports them after the block, like any loss of a sink.
class GatedSink final : public Sink {
public:
    explicit GatedSink(Sink& inner) : inner_(inner) {}

    bool write(const char* line, size_t length) override {
        return !closed_ && inner_.write(line, length);
    }
    const char* name() const override { return inner_.name(); }

    void set_closed(bool closed) { closed_ = closed; }

private:
    Sink& inner_;
    bool closed_ = false;
};

// The writer task's side of the console: writes the replies, and serves a block one line (or
// one chunk of a file) at a time, so the task can go on draining the log queue between steps
// and flash and RTC never wait for a block (ADR-012 §5).
//
// `log dump` (#29) is one reply of several blocks, one per log file, oldest first, with one
// terminator. The USB stays out of the log for the whole reply, between blocks too.
//
// Not thread-safe: owned by the writer task, like Writer. failures() is safe from any task.
//
// While block_open(), the task must call step() and must not take more messages: a reply
// written now would land inside the block.
class ReplyServer {
public:
    ReplyServer(ReplyOutput& output, GatedSink& gate, RamTail& tail)
        : output_(output), gate_(gate), tail_(tail) {}

    // The log in flash, for `log dump`. Without one (the flash did not mount) the dump answers
    // `unsupported`. Call before the writer task starts.
    void attach_archive(LogArchive& archive) { archive_ = &archive; }

    void handle(const ReplyMessage& message);
    void step();
    bool block_open() const { return mode_ != Mode::Idle; }

    // Replies or lines that could not be written since boot (the host stopped reading).
    uint32_t failures() const { return failures_.load(std::memory_order_relaxed); }

private:
    enum class Mode : uint8_t { Idle, Tail, Dump };

    void begin_tail(const char* id);
    void step_tail();
    void begin_dump(const char* id);
    void step_dump();
    // A whole reply line that is not part of a block: an error, the final ok.
    void write_line(size_t length);
    void end_block();
    void count_failure();
    // Tells the host, if it still reads; the host's own timeout covers the rest.
    void send_timeout(const char* id);
    void fail_block();

    ReplyOutput& output_;
    GatedSink& gate_;
    RamTail& tail_;
    LogArchive* archive_ = nullptr;
    char line_[kLineBufferSize] = {};
    char id_[kRequestIdCapacity] = {};
    size_t next_ = 0;
    uint32_t crc_ = 0;
    Mode mode_ = Mode::Idle;
    // The dump: the files as they were at its start, and where it is in them.
    ArchiveFile files_[kMaxStoreFiles] = {};
    size_t file_count_ = 0;
    size_t file_ = 0;        // the file being sent; file_count_ = all sent
    uint32_t offset_ = 0;    // bytes of that file sent
    bool in_block_ = false;  // its #BEGIN went out, its #END not yet
    char chunk_[kDumpChunk] = {};
    bool aborted_ = false;  // the reply `aborted_id_` failed: drop the rest of it
    char aborted_id_[kRequestIdCapacity] = {};
    std::atomic<uint32_t> failures_{0};
};

}  // namespace logcat
