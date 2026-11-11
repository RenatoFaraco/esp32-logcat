// The console's logic on the PC: bytes in, commands executed, replies out, and the writer
// task's side (ReplyServer, RamTail, GatedSink). The FreeRTOS queue is replaced by a vector.

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "logcat/console.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/ram_tail.hpp"
#include "logcat/reply_formatter.hpp"
#include "logcat/reply_server.hpp"
#include "logcat/tag_level_filter.hpp"

using logcat::Console;
using logcat::GatedSink;
using logcat::LineAssembler;
using logcat::LogLevel;
using logcat::RamTail;
using logcat::ReplyFields;
using logcat::ReplyKind;
using logcat::ReplyMessage;
using logcat::ReplyServer;
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
            char key[16];
            std::snprintf(key, sizeof(key), "counter_%02d", i);
            fields.add_uint(key, 4000000000u);
        }
    }
    void set_log_level(const char* tag, LogLevel level) override {
        level_tag = tag;
        platform_level = level;
    }
    void reboot() override {
        reboot_saw = messages.size();
        ++reboots;
    }
    void crash_panic() override { ++panics; }
    void crash_wdt() override { ++wdts; }

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
    std::string text;
    int accept = -1;  // -1 = always
    int calls = 0;
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
        char tag[8];
        std::snprintf(tag, sizeof(tag), "T%zu", i);
        TEST_ASSERT_TRUE(rig.filter.set(tag, LogLevel::Debug));
    }
    rig.send("3 log level NEW D\n");
    TEST_ASSERT_EQUAL_STRING("#! 3 full tag table full (16 tags)\n", rig.platform.lines().c_str());
    TEST_ASSERT_EQUAL_STRING("", rig.platform.level_tag.c_str());
}

void test_log_tail_is_one_message_for_the_writer_task() {
    Rig rig;
    rig.send("4 log tail\n");
    TEST_ASSERT_EQUAL_size_t(1, rig.platform.messages.size());
    TEST_ASSERT_TRUE(rig.platform.messages[0].kind == ReplyKind::Tail);
    TEST_ASSERT_EQUAL_STRING("4", rig.platform.messages[0].id);
}

void test_flash_commands_are_unsupported_for_now() {
    for (const char* request : {"5 log dump\n", "5 log clear\n"}) {
        Rig rig;
        rig.send(request);
        TEST_ASSERT_EQUAL_STRING("#! 5 unsupported needs flash storage (M4)\n",
                                 rig.platform.lines().c_str());
    }
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
                char key[16];
                std::snprintf(key, sizeof(key), "counter_%02d", i);
                fields.add_uint(key, 4000000000u);
            }
        }
        void set_log_level(const char*, LogLevel) override {}
        void reboot() override {}
        void crash_panic() override {}
        void crash_wdt() override {}
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

void test_step_without_a_block_does_nothing() {
    Server s;
    s.server.step();
    TEST_ASSERT_EQUAL_INT(0, s.output.calls);
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
    RUN_TEST(test_log_tail_is_one_message_for_the_writer_task);
    RUN_TEST(test_flash_commands_are_unsupported_for_now);
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
    RUN_TEST(test_step_without_a_block_does_nothing);
    return UNITY_END();
}
