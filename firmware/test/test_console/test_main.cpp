// The console's logic on the PC: bytes in, commands executed, replies out, and the writer
// task's side (ReplyServer, RamTail, GatedSink). The FreeRTOS queue is replaced by a vector.

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../support/memory_fs.hpp"
#include "logcat/console.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/log_archive.hpp"
#include "logcat/ram_tail.hpp"
#include "logcat/reply_formatter.hpp"
#include "logcat/reply_server.hpp"
#include "logcat/rotating_store.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/tag_level_filter.hpp"
#include "logcat/writer.hpp"

using logcat::Console;
using logcat::GatedSink;
using logcat::LineAssembler;
using logcat::LogLevel;
using logcat::RamTail;
using logcat::ReplyFields;
using logcat::ReplyKind;
using logcat::ReplyMessage;
using logcat::ReplyServer;
using logcat::SyncResult;
using logcat::TagLevelFilter;

namespace {

class FakePlatform final : public logcat::ConsolePlatform {
public:
    bool send(const ReplyMessage& message) override {
        if (room == 0) {
            ++refused;
            return false;
        }
        if (room > 0) {
            --room;
        }
        messages.push_back(message);
        return true;
    }
    void fill_info(ReplyFields& fields) override {
        fields.add("fw", "1.2.3");
        fields.add_uint("proto", 1);
    }
    void fill_stats(ReplyFields& fields) override {
        for (int i = 0; i < stat_fields; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "counter_%02d", i);
            fields.add_uint(key, 4000000000u);
        }
    }
    void set_log_level(const char* tag, LogLevel level) override {
        level_tag = tag;
        platform_level = level;
        levels.emplace_back(tag, level);
    }
    void reboot() override {
        reboot_saw = messages.size();
        ++reboots;
    }
    void crash_panic() override { ++panics; }
    void crash_wdt() override { ++wdts; }
    SyncResult sync_log() override {
        ++syncs;
        return sync_result;
    }
    SyncResult sync_result = SyncResult::Ok;
    int syncs = 0;
    logcat::ClearResult clear_log() override {
        ++clears;
        return clear_result;
    }
    logcat::ClearResult clear_result = logcat::ClearResult::Ok;
    int clears = 0;

    std::string lines() const {
        std::string all;
        for (const ReplyMessage& message : messages) {
            all.append(message.line, message.length);
        }
        return all;
    }

    std::vector<ReplyMessage> messages;
    int room = -1;  // -1 = always room
    int refused = 0;
    int stat_fields = 2;
    std::string level_tag;
    LogLevel platform_level = LogLevel::Info;
    std::vector<std::pair<std::string, LogLevel>> levels;
    size_t reboot_saw = 0;
    int reboots = 0;
    int panics = 0;
    int wdts = 0;
};

class FakeOutput final : public logcat::ReplyOutput {
public:
    bool write_reply(const char* line, size_t length) override {
        ++calls;
        if (accept == 0) {
            return false;
        }
        if (accept > 0) {
            --accept;
        }
        text.append(line, length);
        return true;
    }
    bool try_write_reply(const char* line, size_t length) override {
        ++tries;
        return write_reply(line, length);
    }
    std::string text;
    int accept = -1;  // -1 = always
    int calls = 0;
    int tries = 0;
};

class FakeSink final : public logcat::Sink {
public:
    bool write(const char* line, size_t length) override {
        text.append(line, length);
        return true;
    }
    const char* name() const override { return "usb"; }
    std::string text;
};

struct Rig {
    FakePlatform platform;
    TagLevelFilter filter;
    Console console{platform, filter};

    void send(const std::string& text) { console.feed(text.data(), text.size()); }
};

ReplyMessage message_of(ReplyKind kind, const char* id, const std::string& line) {
    ReplyMessage message{};
    message.kind = kind;
    message.length = static_cast<uint8_t>(line.size());
    std::memcpy(message.id, id, std::strlen(id) + 1);
    std::memcpy(message.line, line.data(), line.size());
    return message;
}

struct Server {
    FakeOutput output;
    FakeSink usb;
    GatedSink gate{usb};
    RamTail tail{};
    ReplyServer server{output, gate, tail};

    void add_tail_line(const std::string& line) { tail.write(line.data(), line.size()); }
    void run_block() {
        for (int i = 0; server.block_open() && i < 100; ++i) {
            server.step();
        }
    }
};

// The flash store's side of a dump, on the in-memory file system: what FlashStore does on the
// board, without the flush policy.
class StoreArchive final : public logcat::LogArchive {
public:
    StoreArchive(test_support::MemoryFileSystem& fs, logcat::RotatingStore& store)
        : fs_(fs), store_(store) {}

    bool begin_dump(logcat::ArchiveFile (&files)[logcat::kMaxStoreFiles],
                    size_t& count) override {
        ++begins;
        if (fail_sync) {
            return false;
        }
        store_.flush();
        count = store_.file_count();
        for (size_t i = 0; i < count; ++i) {
            store_.file_name(i, files[i].name);
            files[i].size = store_.file_size(i);
        }
        return true;
    }
    size_t read(const char* name, uint32_t offset, char* out, size_t length) override {
        ++reads;
        return fs_.read(name, offset, out, length);
    }
    void end_dump() override { ++ends; }

    bool fail_sync = false;
    int begins = 0;
    int reads = 0;
    int ends = 0;

private:
    test_support::MemoryFileSystem& fs_;
    logcat::RotatingStore& store_;
};

// A Server with a log in flash: `file_limit` small enough to rotate in a test.
struct DumpServer : Server {
    explicit DumpServer(uint32_t file_limit = 64 * 1024) : store(fs, config_of(file_limit)) {
        server.attach_archive(archive);
    }
    static logcat::StoreConfig config_of(uint32_t file_limit) {
        logcat::StoreConfig config;
        config.file_limit = file_limit;
        return config;
    }
    void log(const std::string& line) { store.write(line.data(), line.size()); }
    // Steps until the output has grown by a `#BEGIN`, `#END`, a chunk or a terminator.
    void steps(int count) {
        for (int i = 0; i < count && server.block_open(); ++i) {
            server.step();
        }
    }

    test_support::MemoryFileSystem fs;
    logcat::RotatingStore store;
    StoreArchive archive{fs, store};
};

std::string crc_of(const std::string& content) {
    char out[9];
    std::snprintf(
        out, sizeof(out), "%08lx",
        static_cast<unsigned long>(logcat::crc32_update(0, content.data(), content.size())));
    return out;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- LineAssembler

void test_assembler_splits_at_either_line_ending() {
    LineAssembler assembler;
    std::vector<std::string> lines;
    for (const char byte : std::string("ab\ncd\r\nef\r")) {
        if (assembler.feed(byte)) {
            lines.emplace_back(assembler.line(), assembler.length());
        }
    }
    TEST_ASSERT_EQUAL_size_t(4, lines.size());
    TEST_ASSERT_EQUAL_STRING("ab", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("cd", lines[1].c_str());
    TEST_ASSERT_EQUAL_STRING("", lines[2].c_str());  // the '\n' of "\r\n": an empty line
    TEST_ASSERT_EQUAL_STRING("ef", lines[3].c_str());
}

void test_assembler_keeps_an_overlong_line_over_the_limit_and_discards_its_tail() {
    LineAssembler assembler;
    for (int i = 0; i < 500; ++i) {
        TEST_ASSERT_FALSE(assembler.feed('x'));
    }
    TEST_ASSERT_TRUE(assembler.feed('\n'));
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxRequestLength + 1, assembler.length());
    // The next line starts clean: nothing of the tail is left.
    assembler.feed('o');
    assembler.feed('k');
    TEST_ASSERT_TRUE(assembler.feed('\n'));
    TEST_ASSERT_EQUAL_size_t(2, assembler.length());
    TEST_ASSERT_EQUAL_MEMORY("ok", assembler.line(), 2);
}

// ---------------------------------------------------------------- Console

void test_a_line_in_pieces_is_executed_once_when_complete() {
    Rig rig;
    rig.send("7 in");
    TEST_ASSERT_EQUAL_size_t(0, rig.platform.messages.size());
    rig.send("fo\n");
    TEST_ASSERT_EQUAL_STRING("#> 7 fw=1.2.3 proto=1\n#. 7 ok\n", rig.platform.lines().c_str());
}

void test_two_requests_in_one_chunk() {
    Rig rig;
    rig.send("1 info\n2 info\n");
    TEST_ASSERT_EQUAL_STRING("#> 1 fw=1.2.3 proto=1\n#. 1 ok\n#> 2 fw=1.2.3 proto=1\n#. 2 ok\n",
                             rig.platform.lines().c_str());
}

void test_crlf_gets_one_reply() {
    Rig rig;
    rig.send("7 info\r\n");
    TEST_ASSERT_EQUAL_size_t(2, rig.platform.messages.size());
}

void test_last_line_of_a_reply_is_the_terminator() {
    Rig rig;
    rig.send("7 info\n");
    TEST_ASSERT_TRUE(rig.platform.messages[0].kind == ReplyKind::Data);
    TEST_ASSERT_TRUE(rig.platform.messages[1].kind == ReplyKind::End);
    TEST_ASSERT_EQUAL_STRING("7", rig.platform.messages[1].id);
}

void test_stats_splits_over_lines_and_every_line_is_valid() {
    Rig rig;
    rig.platform.stat_fields = 20;  // ~21 bytes each: three lines
    rig.send("9999 stats\n");
    const size_t count = rig.platform.messages.size();
    TEST_ASSERT_TRUE(count >= 4);
    size_t fields = 0;
    for (size_t i = 0; i + 1 < count; ++i) {
        const ReplyMessage& message = rig.platform.messages[i];
        TEST_ASSERT_TRUE(message.length <= logcat::kMaxLineLength);
        TEST_ASSERT_EQUAL_CHAR('\n', message.line[message.length - 1]);
        TEST_ASSERT_EQUAL_MEMORY("#> 9999 ", message.line, 8);
        for (size_t j = 0; j < message.length; ++j) {
            fields += message.line[j] == '=' ? 1 : 0;
        }
    }
    TEST_ASSERT_EQUAL_size_t(20, fields);  // none lost at the line break
    TEST_ASSERT_EQUAL_STRING("#. 9999 ok\n", rig.platform.messages[count - 1].line);
}

void test_a_field_with_no_data_still_ends_with_the_terminator() {
    Rig rig;
    rig.platform.stat_fields = 0;
    rig.send("7 stats\n");
    TEST_ASSERT_EQUAL_STRING("#. 7 ok\n", rig.platform.lines().c_str());
}

void test_blank_line_gets_no_reply() {
    Rig rig;
    rig.send("\n   \n\r\n");
    TEST_ASSERT_EQUAL_size_t(0, rig.platform.messages.size());
}

void test_rejected_requests() {
    const struct {
        const char* request;
        const char* start;
    } cases[] = {
        {"7 help\n", "#! 7 bad_command "},
        {"7 info now\n", "#! 7 bad_args "},
        {"info\n", "#! - bad_args "},
        {"07 info\n", "#! - bad_args "},
    };
    for (const auto& item : cases) {
        Rig rig;
        rig.send(item.request);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, rig.platform.messages.size(), item.request);
        TEST_ASSERT_TRUE_MESSAGE(rig.platform.messages[0].kind == ReplyKind::End, item.request);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(item.start, rig.platform.messages[0].line,
                                         std::strlen(item.start), item.request);
    }
}

void test_overlong_request_is_one_error_and_its_tail_is_not_a_request() {
    Rig rig;
    rig.send(std::string(200, ' ') + "7 info\n");  // would parse if the tail were kept
    TEST_ASSERT_EQUAL_size_t(1, rig.platform.messages.size());
    TEST_ASSERT_EQUAL_MEMORY("#! - bad_args", rig.platform.messages[0].line, 13);
}

void test_log_level_sets_the_filter_and_the_platform() {
    Rig rig;
    rig.send("3 log level PRESSURE D\n");
    TEST_ASSERT_TRUE(rig.filter.threshold("PRESSURE") == LogLevel::Debug);
    TEST_ASSERT_EQUAL_STRING("PRESSURE", rig.platform.level_tag.c_str());
    TEST_ASSERT_TRUE(rig.platform.platform_level == LogLevel::Debug);
    TEST_ASSERT_EQUAL_STRING("#. 3 ok\n", rig.platform.lines().c_str());
}

void test_log_level_star_sets_the_default() {
    Rig rig;
    rig.send("3 log level * W\n");
    TEST_ASSERT_TRUE(rig.filter.threshold("ANY") == LogLevel::Warn);
    TEST_ASSERT_EQUAL_size_t(0, rig.filter.size());
}

void test_log_level_none_silences() {
    Rig rig;
    rig.send("3 log level wifi N\n");
    TEST_ASSERT_FALSE(rig.filter.enabled("wifi", LogLevel::Error));
}

void test_log_level_with_a_full_table_is_an_error_and_leaves_the_platform_alone() {
    Rig rig;
    for (size_t i = 0; i < TagLevelFilter::capacity(); ++i) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "T%zu", i);
        TEST_ASSERT_TRUE(rig.filter.set(tag, LogLevel::Debug));
    }
    rig.send("3 log level NEW D\n");
    TEST_ASSERT_EQUAL_STRING("#! 3 full tag table full (16 tags)\n", rig.platform.lines().c_str());
    TEST_ASSERT_EQUAL_STRING("", rig.platform.level_tag.c_str());
}

// ESP-IDF's "*" wipes its per-tag levels; the filter keeps its own, so they go back.
void test_log_level_star_restores_the_platform_levels_of_each_tag() {
    Rig rig;
    rig.send("1 log level HEARTBEAT D\n");
    rig.send("2 log level APP E\n");
    rig.platform.levels.clear();
    rig.send("3 log level * W\n");
    TEST_ASSERT_EQUAL_size_t(3, rig.platform.levels.size());
    TEST_ASSERT_EQUAL_STRING("*", rig.platform.levels[0].first.c_str());  // first, then the tags
    TEST_ASSERT_EQUAL_STRING("HEARTBEAT", rig.platform.levels[1].first.c_str());
    TEST_ASSERT_TRUE(rig.platform.levels[1].second == LogLevel::Debug);
    TEST_ASSERT_EQUAL_STRING("APP", rig.platform.levels[2].first.c_str());
    TEST_ASSERT_TRUE(rig.platform.levels[2].second == LogLevel::Error);
}

void test_log_level_of_one_tag_touches_only_that_tag() {
    Rig rig;
    rig.send("1 log level HEARTBEAT D\n");
    rig.platform.levels.clear();
    rig.send("2 log level APP E\n");
    TEST_ASSERT_EQUAL_size_t(1, rig.platform.levels.size());
}

// A platform that refuses exactly its n-th send and takes the others.
struct RefusesNth final : logcat::ConsolePlatform {
    explicit RefusesNth(int n) : refused_at(n) {}
    bool send(const ReplyMessage& message) override {
        if (++sends == refused_at) {
            return false;
        }
        messages.push_back(message);
        return true;
    }
    void fill_info(ReplyFields& fields) override { fields.add("a", "1"); }
    void fill_stats(ReplyFields&) override {}
    void set_log_level(const char*, LogLevel) override {}
    void reboot() override {}
    void crash_panic() override {}
    void crash_wdt() override {}
    SyncResult sync_log() override { return SyncResult::Ok; }
    logcat::ClearResult clear_log() override { return logcat::ClearResult::Ok; }
    int refused_at;
    int sends = 0;
    std::vector<ReplyMessage> messages;
};

void test_tail_that_finds_no_room_becomes_a_timeout_error() {
    RefusesNth platform(1);
    TagLevelFilter filter;
    Console console(platform, filter);
    console.feed("4 log tail\n", 11);
    TEST_ASSERT_EQUAL_size_t(1, platform.messages.size());
    TEST_ASSERT_EQUAL_STRING("#! 4 timeout reply queue full\n", platform.messages[0].line);
}

void test_ok_that_finds_no_room_becomes_a_timeout_error() {
    RefusesNth platform(2);  // the data line goes, the terminator does not
    TagLevelFilter filter;
    Console console(platform, filter);
    console.feed("7 info\n", 7);
    TEST_ASSERT_EQUAL_size_t(2, platform.messages.size());
    TEST_ASSERT_EQUAL_STRING("#! 7 timeout reply queue full\n", platform.messages[1].line);
}

void test_request_of_exactly_the_limit_runs_and_one_more_does_not() {
    const std::string at_limit = "7 info" + std::string(logcat::kMaxRequestLength - 6, ' ');
    Rig ok;
    ok.send(at_limit + "\n");
    TEST_ASSERT_EQUAL_STRING("#> 7 fw=1.2.3 proto=1\n#. 7 ok\n", ok.platform.lines().c_str());
    Rig over;
    over.send(at_limit + " \n");
    TEST_ASSERT_EQUAL_MEMORY("#! - bad_args", over.platform.messages[0].line, 13);
}

void test_log_tail_is_one_message_for_the_writer_task() {
    Rig rig;
    rig.send("4 log tail\n");
    TEST_ASSERT_EQUAL_size_t(1, rig.platform.messages.size());
    TEST_ASSERT_TRUE(rig.platform.messages[0].kind == ReplyKind::Tail);
    TEST_ASSERT_EQUAL_STRING("4", rig.platform.messages[0].id);
}

// `log sync` (#26) answers only after the sync: ok means the log is in flash.
void test_log_sync_answers_ok_once_synced() {
    Rig rig;
    rig.send("5 log sync\n");
    TEST_ASSERT_EQUAL(1, rig.platform.syncs);
    TEST_ASSERT_EQUAL_STRING("#. 5 ok\n", rig.platform.lines().c_str());
}

void test_log_sync_that_failed_is_an_error() {
    Rig rig;
    rig.platform.sync_result = logcat::SyncResult::Failed;
    rig.send("5 log sync\n");
    TEST_ASSERT_EQUAL_STRING("#! 5 failed sync failed or timed out: lines may not be in flash\n",
                             rig.platform.lines().c_str());
}

void test_log_sync_without_flash_is_unsupported() {
    Rig rig;
    rig.platform.sync_result = logcat::SyncResult::Unsupported;
    rig.send("5 log sync\n");
    TEST_ASSERT_EQUAL_STRING("#! 5 unsupported no flash storage\n", rig.platform.lines().c_str());
}

void test_log_dump_is_one_message_for_the_writer_task() {
    Rig rig;
    rig.send("4 log dump\n");
    TEST_ASSERT_EQUAL_size_t(1, rig.platform.messages.size());
    TEST_ASSERT_TRUE(rig.platform.messages[0].kind == ReplyKind::Dump);
    TEST_ASSERT_EQUAL_STRING("4", rig.platform.messages[0].id);
}

// `log clear` (#29) answers after the files are gone: ok means the log starts over.
void test_log_clear_answers_ok_once_cleared() {
    Rig rig;
    rig.send("5 log clear\n");
    TEST_ASSERT_EQUAL(1, rig.platform.clears);
    TEST_ASSERT_EQUAL_STRING("#. 5 ok\n", rig.platform.lines().c_str());
}

void test_log_clear_during_a_dump_is_busy() {
    Rig rig;
    rig.platform.clear_result = logcat::ClearResult::Busy;
    rig.send("5 log clear\n");
    TEST_ASSERT_EQUAL_STRING("#! 5 busy a log dump is being sent: try again after it\n",
                             rig.platform.lines().c_str());
}

void test_log_clear_that_failed_is_an_error() {
    Rig rig;
    rig.platform.clear_result = logcat::ClearResult::Failed;
    rig.send("5 log clear\n");
    TEST_ASSERT_EQUAL_STRING("#! 5 failed clear failed or timed out: some files may be left\n",
                             rig.platform.lines().c_str());
}

void test_log_clear_without_flash_is_unsupported() {
    Rig rig;
    rig.platform.clear_result = logcat::ClearResult::Unsupported;
    rig.send("5 log clear\n");
    TEST_ASSERT_EQUAL_STRING("#! 5 unsupported no flash storage\n", rig.platform.lines().c_str());
}

void test_reboot_replies_before_restarting() {
    Rig rig;
    rig.send("6 reboot\n");
    TEST_ASSERT_EQUAL_INT(1, rig.platform.reboots);
    TEST_ASSERT_EQUAL_size_t(1, rig.platform.reboot_saw);  // the "ok" was already queued
    TEST_ASSERT_EQUAL_STRING("#. 6 ok\n", rig.platform.lines().c_str());
}

void test_crash_does_not_reply() {
    Rig rig;
    rig.send("6 crash panic\n6 crash wdt\n");
    TEST_ASSERT_EQUAL_INT(1, rig.platform.panics);
    TEST_ASSERT_EQUAL_INT(1, rig.platform.wdts);
    TEST_ASSERT_EQUAL_size_t(0, rig.platform.messages.size());
}

void test_full_queue_turns_the_reply_into_a_timeout_error() {
    Rig rig;
    rig.platform.stat_fields = 20;
    rig.platform.room = 0;  // not even the first data line fits
    rig.send("7 stats\n");
    rig.platform.room = -1;
    TEST_ASSERT_EQUAL_size_t(0, rig.platform.messages.size());  // terminator also refused
    // A later request starts clean.
    rig.send("8 info\n");
    TEST_ASSERT_EQUAL_STRING("#> 8 fw=1.2.3 proto=1\n#. 8 ok\n", rig.platform.lines().c_str());
}

void test_queue_full_midway_stops_the_lines_and_tries_a_timeout_terminator() {
    struct Once final : logcat::ConsolePlatform {
        bool send(const ReplyMessage& message) override {
            ++sends;
            if (sends == 2) {
                return false;  // the second data line finds no room, then room comes back
            }
            messages.push_back(message);
            return true;
        }
        void fill_info(ReplyFields&) override {}
        void fill_stats(ReplyFields& fields) override {
            for (int i = 0; i < 30; ++i) {
                char key[32];
                std::snprintf(key, sizeof(key), "counter_%02d", i);
                fields.add_uint(key, 4000000000u);
            }
        }
        void set_log_level(const char*, LogLevel) override {}
        void reboot() override {}
        void crash_panic() override {}
        void crash_wdt() override {}
        SyncResult sync_log() override { return SyncResult::Ok; }
        logcat::ClearResult clear_log() override { return logcat::ClearResult::Ok; }
        std::vector<ReplyMessage> messages;
        int sends = 0;
    } platform;
    TagLevelFilter filter;
    Console console(platform, filter);
    console.feed("7 stats\n", 8);
    TEST_ASSERT_EQUAL_size_t(2, platform.messages.size());  // first data line, then the error
    TEST_ASSERT_EQUAL_STRING("#! 7 timeout reply queue full\n", platform.messages[1].line);
    TEST_ASSERT_TRUE(platform.messages[1].kind == ReplyKind::End);
}

// ---------------------------------------------------------------- RamTail

void test_tail_keeps_the_last_lines_in_order() {
    RamTail tail{};
    for (size_t i = 0; i < logcat::kRamTailLines + 3; ++i) {
        const std::string line = "line " + std::to_string(i) + "\n";
        TEST_ASSERT_TRUE(tail.write(line.data(), line.size()));
    }
    TEST_ASSERT_EQUAL_size_t(logcat::kRamTailLines, tail.size());
    const logcat::TailLine* oldest = tail.at(0);
    TEST_ASSERT_EQUAL_MEMORY("line 3\n", oldest->text, oldest->length);
}

void test_tail_counts_its_bytes() {
    RamTail tail{};
    TEST_ASSERT_EQUAL_UINT32(0, tail.total_bytes());
    tail.write("ab\n", 3);
    tail.write("cde\n", 4);
    TEST_ASSERT_EQUAL_UINT32(7, tail.total_bytes());
}

void test_tail_holds_a_line_of_the_maximum_size() {
    RamTail tail{};
    const std::string line(logcat::kMaxLineLength, 'x');
    TEST_ASSERT_TRUE(tail.write(line.data(), line.size()));
    TEST_ASSERT_EQUAL_UINT32(logcat::kMaxLineLength, tail.total_bytes());
}

void test_frozen_tail_accepts_counts_and_does_not_change() {
    RamTail tail{};
    tail.write("a\n", 2);
    tail.freeze();
    TEST_ASSERT_TRUE(tail.write("b\n", 2));  // accepted: not a failing sink
    TEST_ASSERT_EQUAL_size_t(1, tail.size());
    TEST_ASSERT_EQUAL_UINT32(1, tail.skipped());
    tail.unfreeze();
    tail.write("c\n", 2);
    TEST_ASSERT_EQUAL_size_t(2, tail.size());
    TEST_ASSERT_EQUAL_UINT32(1, tail.skipped());
}

// ---------------------------------------------------------------- ReplyServer

void test_plain_lines_go_out_as_they_are() {
    Server s;
    s.server.handle(message_of(ReplyKind::Data, "7", "#> 7 a=1\n"));
    s.server.handle(message_of(ReplyKind::End, "7", "#. 7 ok\n"));
    TEST_ASSERT_EQUAL_STRING("#> 7 a=1\n#. 7 ok\n", s.output.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, s.server.failures());
    TEST_ASSERT_FALSE(s.server.block_open());
}

void test_tail_block_is_counted_in_bytes_with_its_crc() {
    Server s;
    s.add_tail_line("first\n");
    s.add_tail_line("second\n");
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    TEST_ASSERT_TRUE(s.server.block_open());
    s.run_block();
    TEST_ASSERT_FALSE(s.server.block_open());
    const std::string content = "first\nsecond\n";
    const std::string expected =
        "#BEGIN 4 ram 13\n" + content + "#END 4 " + crc_of(content) + "\n#. 4 ok\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), s.output.text.c_str());
}

void test_empty_tail_is_an_empty_block() {
    Server s;
    s.server.handle(message_of(ReplyKind::Tail, "-", ""));
    s.run_block();
    TEST_ASSERT_EQUAL_STRING("#BEGIN - ram 0\n#END - 00000000\n#. - ok\n", s.output.text.c_str());
}

void test_block_is_served_one_line_per_step() {
    Server s;
    s.add_tail_line("a\n");
    s.add_tail_line("b\n");
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    TEST_ASSERT_EQUAL_INT(1, s.output.calls);  // #BEGIN
    s.server.step();
    TEST_ASSERT_EQUAL_INT(2, s.output.calls);
    s.server.step();
    TEST_ASSERT_EQUAL_INT(3, s.output.calls);
}

void test_usb_is_out_of_the_log_while_the_block_is_open() {
    Server s;
    s.add_tail_line("a\n");
    TEST_ASSERT_TRUE(s.gate.write("before\n", 7));
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    TEST_ASSERT_FALSE(s.gate.write("during\n", 7));  // refused: the Writer counts it
    s.run_block();
    TEST_ASSERT_TRUE(s.gate.write("after\n", 6));
    TEST_ASSERT_EQUAL_STRING("before\nafter\n", s.usb.text.c_str());
}

void test_the_tail_does_not_change_during_its_block() {
    Server s;
    s.add_tail_line("a\n");
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    s.add_tail_line("live\n");  // a line logged while the block is open
    s.run_block();
    const std::string expected = "#BEGIN 4 ram 2\na\n#END 4 " + crc_of("a\n") + "\n#. 4 ok\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), s.output.text.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, s.tail.skipped());
    s.add_tail_line("again\n");  // and it is back afterwards
    TEST_ASSERT_EQUAL_size_t(2, s.tail.size());
}

void test_a_failure_inside_the_block_closes_it_with_an_error_and_no_end() {
    Server s;
    s.add_tail_line("a\n");
    s.add_tail_line("b\n");
    s.output.accept = 2;  // #BEGIN and the first line
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    s.server.step();
    s.output.accept = 0;
    s.server.step();  // the second line times out
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL_UINT32(1, s.server.failures());
    TEST_ASSERT_TRUE(s.output.text.find("#END") == std::string::npos);
    TEST_ASSERT_TRUE(s.gate.write("x\n", 2));  // the log is back on the USB
    TEST_ASSERT_EQUAL_size_t(2, s.tail.size());
    s.add_tail_line("c\n");
    TEST_ASSERT_EQUAL_size_t(3, s.tail.size());  // not frozen forever
}

void test_failure_inside_the_block_sends_the_error_when_it_fits() {
    Server s;
    s.add_tail_line("a\n");
    int writes = 0;
    struct OneFailure final : logcat::ReplyOutput {
        explicit OneFailure(int* counter) : count(counter) {}
        bool write_reply(const char* line, size_t length) override {
            ++*count;
            if (*count == 2) {
                return false;
            }
            text.append(line, length);
            return true;
        }
        bool try_write_reply(const char* line, size_t length) override {
            return write_reply(line, length);
        }
        int* count;
        std::string text;
    } output(&writes);
    ReplyServer server(output, s.gate, s.tail);
    server.handle(message_of(ReplyKind::Tail, "4", ""));
    server.step();
    TEST_ASSERT_EQUAL_STRING("#BEGIN 4 ram 2\n#! 4 timeout host not reading\n",
                             output.text.c_str());
}

void test_failure_at_begin_leaves_nothing_open() {
    Server s;
    s.output.accept = 0;
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL_UINT32(1, s.server.failures());
    TEST_ASSERT_TRUE(s.gate.write("x\n", 2));
}

void test_failure_at_end_marker_closes_the_block() {
    Server s;
    s.output.accept = 1;  // only #BEGIN: the tail is empty, so #END comes next
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    s.output.accept = 0;
    s.server.step();
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL_UINT32(1, s.server.failures());
}

void test_failed_data_line_drops_the_rest_of_that_reply() {
    Server s;
    s.output.accept = 0;
    s.server.handle(message_of(ReplyKind::Data, "7", "#> 7 a=1\n"));
    TEST_ASSERT_EQUAL_UINT32(1, s.server.failures());
    s.output.accept = -1;  // the host is back, but this reply is already broken
    s.server.handle(message_of(ReplyKind::Data, "7", "#> 7 b=2\n"));
    s.server.handle(message_of(ReplyKind::End, "7", "#. 7 ok\n"));
    TEST_ASSERT_EQUAL_STRING("", s.output.text.c_str());
    s.server.handle(message_of(ReplyKind::End, "8", "#. 8 ok\n"));
    TEST_ASSERT_EQUAL_STRING("#. 8 ok\n", s.output.text.c_str());
}

void test_same_id_after_the_terminator_is_a_new_reply() {
    Server s;
    s.output.accept = 0;
    s.server.handle(message_of(ReplyKind::Data, "-", "#> - a=1\n"));
    s.output.accept = -1;
    s.server.handle(message_of(ReplyKind::End, "-", "#. - ok\n"));  // dropped: closes the reply
    s.server.handle(message_of(ReplyKind::End, "-", "#. - ok\n"));  // the next request's
    TEST_ASSERT_EQUAL_STRING("#. - ok\n", s.output.text.c_str());
}

void test_failed_terminator_does_not_swallow_the_next_reply() {
    Server s;
    s.output.accept = 0;
    s.server.handle(message_of(ReplyKind::End, "7", "#. 7 ok\n"));
    s.output.accept = -1;
    s.server.handle(message_of(ReplyKind::End, "7", "#. 7 ok\n"));
    TEST_ASSERT_EQUAL_STRING("#. 7 ok\n", s.output.text.c_str());
}

void test_failed_write_tells_the_host_when_it_can() {
    int writes = 0;
    struct OneFailure final : logcat::ReplyOutput {
        explicit OneFailure(int* counter) : count(counter) {}
        bool write_reply(const char* line, size_t length) override {
            if (++*count == 1) {
                return false;
            }
            text.append(line, length);
            return true;
        }
        bool try_write_reply(const char* line, size_t length) override {
            return write_reply(line, length);
        }
        int* count;
        std::string text;
    } output(&writes);
    FakeSink usb;
    GatedSink gate(usb);
    RamTail tail{};
    ReplyServer server(output, gate, tail);
    server.handle(message_of(ReplyKind::Data, "7", "#> 7 a=1\n"));
    TEST_ASSERT_EQUAL_STRING("#! 7 timeout host not reading\n", output.text.c_str());
}

void test_the_timeout_error_does_not_wait_again() {
    Server s;
    s.output.accept = 0;
    s.server.handle(message_of(ReplyKind::End, "7", "#. 7 ok\n"));
    TEST_ASSERT_EQUAL_INT(1, s.output.tries);  // and only that one try
    TEST_ASSERT_EQUAL_INT(2, s.output.calls);  // the write, then the try (which forwards)
}

void test_refused_terminator_after_end_closes_the_block_and_counts() {
    Server s;
    s.output.accept = 2;  // #BEGIN and #END: the tail is empty
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    s.server.step();
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL_UINT32(1, s.server.failures());
    TEST_ASSERT_TRUE(s.gate.write("x\n", 2));
}

void test_full_block_after_wrap_counts_every_byte() {
    Server s;
    std::vector<std::string> lines;
    for (size_t i = 0; i < logcat::kRamTailLines + 5; ++i) {
        std::string line(logcat::kMaxLineLength - 1, static_cast<char>('a' + i % 26));
        line += "\n";
        s.add_tail_line(line);
        lines.push_back(line);
    }
    std::string content;
    for (size_t i = 5; i < lines.size(); ++i) {
        content += lines[i];
    }
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    s.run_block();
    const std::string expected = "#BEGIN 4 ram " + std::to_string(content.size()) + "\n" + content +
                                 "#END 4 " + crc_of(content) + "\n#. 4 ok\n";
    TEST_ASSERT_TRUE(s.output.text == expected);
}

// The real Writer with the gate and the tail: what the board wires together.
void test_writer_with_gate_and_tail_during_a_block() {
    struct Clock final : logcat::Clock {
        uint32_t uptime_ms() override { return 1; }
    } clock;
    logcat::Sequencer sequencer;
    logcat::Writer writer(sequencer, clock);
    Server s;
    writer.add_sink(s.gate);
    writer.add_sink(s.tail);
    logcat::LogRecord record{};
    record.level = LogLevel::Info;
    record.set_tag("APP");
    record.set_message("before");
    writer.write(record);
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    const size_t usb_before = s.usb.text.size();

    record.set_message("during");
    writer.write(record);
    writer.idle();  // a report now would land inside the block
    TEST_ASSERT_EQUAL_size_t(usb_before, s.usb.text.size());
    TEST_ASSERT_EQUAL_UINT32(1, writer.refused_total(0));
    TEST_ASSERT_EQUAL_UINT32(0, writer.refused_total(1));  // the frozen tail is not failing
    TEST_ASSERT_EQUAL_UINT32(1, s.tail.skipped());

    s.run_block();
    record.set_message("after");
    writer.write(record);
    writer.idle();
    TEST_ASSERT_TRUE(s.usb.text.find("after") != std::string::npos);
    TEST_ASSERT_TRUE(s.usb.text.find("1 lines not sent to usb") != std::string::npos);
    TEST_ASSERT_TRUE(s.usb.text.find("during") == std::string::npos);
}

void test_a_second_block_starts_its_crc_from_zero() {
    Server s;
    s.add_tail_line("a\n");
    s.server.handle(message_of(ReplyKind::Tail, "4", ""));
    s.run_block();
    s.output.text.clear();
    s.server.handle(message_of(ReplyKind::Tail, "5", ""));
    s.run_block();
    const std::string expected = "#BEGIN 5 ram 2\na\n#END 5 " + crc_of("a\n") + "\n#. 5 ok\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), s.output.text.c_str());
}

// The terminator of the broken reply never came: another id ends the abort, so the broken
// reply's id is usable again.
void test_another_id_ends_an_abort() {
    Server s;
    s.output.accept = 0;
    s.server.handle(message_of(ReplyKind::Data, "7", "#> 7 a=1\n"));
    s.output.accept = -1;
    s.server.handle(message_of(ReplyKind::End, "8", "#. 8 ok\n"));
    s.server.handle(message_of(ReplyKind::End, "7", "#. 7 ok\n"));
    TEST_ASSERT_EQUAL_STRING("#. 8 ok\n#. 7 ok\n", s.output.text.c_str());
}

void test_step_without_a_block_does_nothing() {
    Server s;
    s.server.step();
    TEST_ASSERT_EQUAL_INT(0, s.output.calls);
}

// ---------------------------------------------------------------- ReplyServer: log dump (#29)

void test_dump_without_flash_is_unsupported() {
    Server s;  // no archive attached: the flash did not mount
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL_STRING("#! 4 unsupported no flash storage\n", s.output.text.c_str());
}

void test_dump_with_no_files_is_only_the_terminator() {
    DumpServer s;  // store never begun: no file at all
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    s.run_block();
    TEST_ASSERT_EQUAL_STRING("#. 4 ok\n", s.output.text.c_str());
    TEST_ASSERT_EQUAL(1, s.archive.ends);
}

void test_dump_sends_one_block_per_file_oldest_first_and_one_terminator() {
    DumpServer s(20);
    s.store.begin();
    s.log("one 456789 123456789\n");  // 21 bytes: fills log_000 alone
    s.log("two\n");                     // log_001
    s.log("three\n");                   // log_001
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    s.run_block();
    const std::string first = "one 456789 123456789\n";
    const std::string second = "two\nthree\n";
    const std::string expected = "#BEGIN 4 log_000.txt 21\n" + first + "#END 4 " + crc_of(first) +
                                 "\n#BEGIN 4 log_001.txt 10\n" + second + "#END 4 " +
                                 crc_of(second) + "\n#. 4 ok\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), s.output.text.c_str());
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL(1, s.archive.begins);
    TEST_ASSERT_EQUAL(1, s.archive.ends);
}

void test_dump_sends_a_file_in_chunks_with_bytes_as_they_are() {
    DumpServer s;
    std::string content;
    for (size_t i = 0; i < 1300; ++i) {
        content += static_cast<char>(i % 256);  // NUL, 0xFF, no newline at the end: as they are
    }
    s.fs.put("log_000.txt", content);
    s.store.begin();  // finds the file; its torn tail is repaired only at the next line
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    s.run_block();
    const std::string expected = "#BEGIN 4 log_000.txt 1300\n" + content + "#END 4 " +
                                 crc_of(content) + "\n#. 4 ok\n";
    TEST_ASSERT_TRUE(s.output.text == expected);
    TEST_ASSERT_EQUAL(3, s.archive.reads);  // 512 + 512 + 276
}

void test_lines_logged_during_the_dump_are_not_in_it() {
    DumpServer s;
    s.store.begin();
    s.log("before\n");
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    s.steps(1);  // #BEGIN with the size taken at the start
    s.log("during\n");
    s.run_block();
    const std::string expected =
        "#BEGIN 4 log_000.txt 7\nbefore\n#END 4 " + crc_of("before\n") + "\n#. 4 ok\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), s.output.text.c_str());
    s.store.flush();
    TEST_ASSERT_EQUAL_STRING("before\nduring\n", s.fs.content("log_000.txt").c_str());
}

void test_a_file_that_changes_under_the_dump_ends_it_with_failed() {
    DumpServer s(20);
    s.store.begin();
    s.log("one 456789 123456789\n");
    s.log("two\n");
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    s.steps(4);  // the first block whole, then #BEGIN of the second
    s.fs.fail_next_read = true;  // as if the rotation had removed it
    s.run_block();
    const std::string first = "one 456789 123456789\n";
    const std::string expected = "#BEGIN 4 log_000.txt 21\n" + first + "#END 4 " + crc_of(first) +
                                 "\n#BEGIN 4 log_001.txt 4\n#! 4 failed log rotated during dump\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), s.output.text.c_str());
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL(1, s.archive.ends);
    TEST_ASSERT_EQUAL_UINT32(0, s.server.failures());  // the host was reading
    TEST_ASSERT_TRUE(s.gate.write("x\n", 2));
}

void test_dump_that_the_host_stops_reading_ends_with_a_timeout() {
    DumpServer s;
    s.store.begin();
    s.log("a\n");
    s.output.accept = 1;  // only the #BEGIN
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    s.run_block();
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL_UINT32(1, s.server.failures());
    TEST_ASSERT_EQUAL(1, s.archive.ends);
    TEST_ASSERT_TRUE(s.gate.write("x\n", 2));
}

void test_a_sync_that_fails_before_the_dump_is_an_error() {
    DumpServer s;
    s.store.begin();
    s.archive.fail_sync = true;
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    TEST_ASSERT_FALSE(s.server.block_open());
    TEST_ASSERT_EQUAL_STRING("#! 4 failed sync failed: the dump would miss lines\n",
                             s.output.text.c_str());
    TEST_ASSERT_EQUAL(1, s.archive.ends);
    TEST_ASSERT_TRUE(s.gate.write("x\n", 2));
}

void test_usb_is_out_of_the_log_for_the_whole_dump() {
    DumpServer s(20);
    s.store.begin();
    s.log("one 456789 123456789\n");
    s.log("two\n");
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    TEST_ASSERT_FALSE(s.gate.write("x\n", 2));
    s.steps(3);  // the first block is closed, the second not yet open
    TEST_ASSERT_FALSE(s.gate.write("x\n", 2));
    s.run_block();
    TEST_ASSERT_TRUE(s.gate.write("x\n", 2));
}

void test_the_tail_keeps_taking_lines_during_a_dump() {
    DumpServer s;
    s.store.begin();
    s.log("a\n");
    s.server.handle(message_of(ReplyKind::Dump, "4", ""));
    s.add_tail_line("live\n");
    TEST_ASSERT_EQUAL_size_t(1, s.tail.size());  // only the tail's own block freezes it
    s.run_block();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_assembler_splits_at_either_line_ending);
    RUN_TEST(test_assembler_keeps_an_overlong_line_over_the_limit_and_discards_its_tail);
    RUN_TEST(test_a_line_in_pieces_is_executed_once_when_complete);
    RUN_TEST(test_two_requests_in_one_chunk);
    RUN_TEST(test_crlf_gets_one_reply);
    RUN_TEST(test_last_line_of_a_reply_is_the_terminator);
    RUN_TEST(test_stats_splits_over_lines_and_every_line_is_valid);
    RUN_TEST(test_a_field_with_no_data_still_ends_with_the_terminator);
    RUN_TEST(test_blank_line_gets_no_reply);
    RUN_TEST(test_rejected_requests);
    RUN_TEST(test_overlong_request_is_one_error_and_its_tail_is_not_a_request);
    RUN_TEST(test_log_level_sets_the_filter_and_the_platform);
    RUN_TEST(test_log_level_star_sets_the_default);
    RUN_TEST(test_log_level_none_silences);
    RUN_TEST(test_log_level_with_a_full_table_is_an_error_and_leaves_the_platform_alone);
    RUN_TEST(test_log_level_star_restores_the_platform_levels_of_each_tag);
    RUN_TEST(test_log_level_of_one_tag_touches_only_that_tag);
    RUN_TEST(test_tail_that_finds_no_room_becomes_a_timeout_error);
    RUN_TEST(test_ok_that_finds_no_room_becomes_a_timeout_error);
    RUN_TEST(test_request_of_exactly_the_limit_runs_and_one_more_does_not);
    RUN_TEST(test_log_tail_is_one_message_for_the_writer_task);
    RUN_TEST(test_log_dump_is_one_message_for_the_writer_task);
    RUN_TEST(test_log_clear_answers_ok_once_cleared);
    RUN_TEST(test_log_clear_during_a_dump_is_busy);
    RUN_TEST(test_log_clear_that_failed_is_an_error);
    RUN_TEST(test_log_clear_without_flash_is_unsupported);
    RUN_TEST(test_dump_without_flash_is_unsupported);
    RUN_TEST(test_dump_with_no_files_is_only_the_terminator);
    RUN_TEST(test_dump_sends_one_block_per_file_oldest_first_and_one_terminator);
    RUN_TEST(test_dump_sends_a_file_in_chunks_with_bytes_as_they_are);
    RUN_TEST(test_lines_logged_during_the_dump_are_not_in_it);
    RUN_TEST(test_a_file_that_changes_under_the_dump_ends_it_with_failed);
    RUN_TEST(test_dump_that_the_host_stops_reading_ends_with_a_timeout);
    RUN_TEST(test_a_sync_that_fails_before_the_dump_is_an_error);
    RUN_TEST(test_usb_is_out_of_the_log_for_the_whole_dump);
    RUN_TEST(test_the_tail_keeps_taking_lines_during_a_dump);
    RUN_TEST(test_log_sync_answers_ok_once_synced);
    RUN_TEST(test_log_sync_that_failed_is_an_error);
    RUN_TEST(test_log_sync_without_flash_is_unsupported);
    RUN_TEST(test_reboot_replies_before_restarting);
    RUN_TEST(test_crash_does_not_reply);
    RUN_TEST(test_full_queue_turns_the_reply_into_a_timeout_error);
    RUN_TEST(test_queue_full_midway_stops_the_lines_and_tries_a_timeout_terminator);
    RUN_TEST(test_tail_keeps_the_last_lines_in_order);
    RUN_TEST(test_tail_counts_its_bytes);
    RUN_TEST(test_tail_holds_a_line_of_the_maximum_size);
    RUN_TEST(test_frozen_tail_accepts_counts_and_does_not_change);
    RUN_TEST(test_plain_lines_go_out_as_they_are);
    RUN_TEST(test_tail_block_is_counted_in_bytes_with_its_crc);
    RUN_TEST(test_empty_tail_is_an_empty_block);
    RUN_TEST(test_block_is_served_one_line_per_step);
    RUN_TEST(test_usb_is_out_of_the_log_while_the_block_is_open);
    RUN_TEST(test_the_tail_does_not_change_during_its_block);
    RUN_TEST(test_a_failure_inside_the_block_closes_it_with_an_error_and_no_end);
    RUN_TEST(test_failure_inside_the_block_sends_the_error_when_it_fits);
    RUN_TEST(test_failure_at_begin_leaves_nothing_open);
    RUN_TEST(test_failure_at_end_marker_closes_the_block);
    RUN_TEST(test_failed_data_line_drops_the_rest_of_that_reply);
    RUN_TEST(test_same_id_after_the_terminator_is_a_new_reply);
    RUN_TEST(test_failed_terminator_does_not_swallow_the_next_reply);
    RUN_TEST(test_failed_write_tells_the_host_when_it_can);
    RUN_TEST(test_the_timeout_error_does_not_wait_again);
    RUN_TEST(test_refused_terminator_after_end_closes_the_block_and_counts);
    RUN_TEST(test_full_block_after_wrap_counts_every_byte);
    RUN_TEST(test_writer_with_gate_and_tail_during_a_block);
    RUN_TEST(test_a_second_block_starts_its_crc_from_zero);
    RUN_TEST(test_another_id_ends_an_abort);
    RUN_TEST(test_step_without_a_block_does_nothing);
    return UNITY_END();
}
