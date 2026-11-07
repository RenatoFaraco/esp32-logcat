#include <unity.h>

#include <cstdio>
#include <cstring>

#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/writer.hpp"

using logcat::LogLevel;
using logcat::LogRecord;
using logcat::Sequencer;
using logcat::Writer;

namespace {

class FakeClock : public logcat::Clock {
public:
    uint32_t now = 1234;
    uint32_t uptime_ms() override { return now; }
};

// Records every line it accepts; refuses while `refusing` is set (no host reading).
class FakeSink : public logcat::Sink {
public:
    explicit FakeSink(const char* sink_name) : name_(sink_name) {}

    bool write(const char* line, size_t length) override {
        ++calls;
        if (refusing) {
            return false;
        }
        if (count < kMax) {
            TEST_ASSERT_TRUE(length < sizeof(lines[0]));
            std::memcpy(lines[count], line, length);
            lines[count][length] = '\0';
            lengths[count] = length;
            ++count;
        }
        return true;
    }
    const char* name() const override { return name_; }

    LogRecord parsed(size_t index) const {
        LogRecord record{};
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(logcat::ParseStatus::Ok),
            static_cast<int>(logcat::parse_line(lines[index], lengths[index], record)));
        return record;
    }

    static constexpr size_t kMax = 16;
    char lines[kMax][logcat::kLineBufferSize] = {};
    size_t lengths[kMax] = {};
    size_t count = 0;
    size_t calls = 0;
    bool refusing = false;

private:
    const char* name_;
};

LogRecord make_record(uint32_t seq, const char* message, LogLevel level = LogLevel::Info) {
    LogRecord record{};
    record.seq = seq;
    record.uptime_ms = 500 + seq;
    record.level = level;
    record.set_tag("APP");
    record.set_message(message);
    return record;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_write_formats_once_and_reaches_every_sink() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    FakeSink console("console");
    TEST_ASSERT_TRUE(writer.add_sink(usb));
    TEST_ASSERT_TRUE(writer.add_sink(console));

    writer.write(make_record(7, "hello"));

    TEST_ASSERT_EQUAL_size_t(1, usb.count);
    TEST_ASSERT_EQUAL_size_t(1, console.count);
    TEST_ASSERT_EQUAL_STRING("0000:000007 0000000507 I APP: hello\n", usb.lines[0]);
    TEST_ASSERT_EQUAL_STRING(usb.lines[0], console.lines[0]);
}

void test_idle_without_losses_writes_nothing() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    writer.add_sink(usb);
    writer.idle();
    writer.idle();
    TEST_ASSERT_EQUAL_size_t(0, usb.calls);
}

// Queue full on the callers' side: the next idle() reports it with a seq of its own, so the
// host sees the gap and, right after it, the explanation.
void test_queue_drops_are_reported_with_their_own_seq() {
    Sequencer sequencer;
    FakeClock clock;
    clock.now = 99000;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    writer.add_sink(usb);
    for (int i = 0; i < 3; ++i) {
        sequencer.next();  // lines 0..2 got a seq, then the queue was full
        sequencer.record_drop();
    }
    writer.idle();
    TEST_ASSERT_EQUAL_size_t(1, usb.count);
    TEST_ASSERT_EQUAL_STRING("0000:000003 0000099000 W LOGCAT: dropped 3 lines: log queue full\n",
                             usb.lines[0]);
    writer.idle();  // reported once
    TEST_ASSERT_EQUAL_size_t(1, usb.count);
}

void test_boot_is_stamped_on_reports() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    writer.set_boot(42);
    FakeSink usb("usb");
    writer.add_sink(usb);
    sequencer.record_drop();
    writer.idle();
    TEST_ASSERT_EQUAL_UINT32(42, usb.parsed(0).boot);
}

void test_refused_lines_are_counted_and_reported_after_recovery() {
    Sequencer sequencer(10);
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    writer.add_sink(usb);

    usb.refusing = true;  // nobody reads the native port
    writer.write(make_record(1, "a"));
    writer.write(make_record(2, "b"));
    TEST_ASSERT_EQUAL_UINT32(2, writer.unsent(0));
    writer.idle();  // still refusing: no report, no seq burnt
    TEST_ASSERT_EQUAL_UINT32(2, writer.unsent(0));
    TEST_ASSERT_EQUAL_UINT32(10, sequencer.next());  // nothing consumed while refusing

    usb.refusing = false;
    writer.write(make_record(12, "c"));
    writer.idle();
    TEST_ASSERT_EQUAL_size_t(2, usb.count);
    TEST_ASSERT_EQUAL_STRING("c", usb.parsed(0).msg);
    const LogRecord report = usb.parsed(1);
    TEST_ASSERT_EQUAL_STRING("LOGCAT", report.tag);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LogLevel::Warn),
                            static_cast<uint8_t>(report.level));
    TEST_ASSERT_EQUAL_STRING("2 lines not sent to usb: no reader", report.msg);
    TEST_ASSERT_EQUAL_UINT32(11, report.seq);
    TEST_ASSERT_EQUAL_UINT32(0, writer.unsent(0));
}

// One sink refusing must not cost the others anything, and the report reaches all of them
// (otherwise the healthy sink would see a hole in seq).
void test_a_refusing_sink_does_not_affect_the_others() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    FakeSink console("console");
    writer.add_sink(usb);
    writer.add_sink(console);

    usb.refusing = true;
    writer.write(make_record(0, "x"));
    TEST_ASSERT_EQUAL_size_t(1, console.count);
    TEST_ASSERT_EQUAL_UINT32(0, writer.unsent(1));

    usb.refusing = false;
    writer.write(make_record(1, "y"));
    writer.idle();
    TEST_ASSERT_EQUAL_size_t(3, console.count);
    TEST_ASSERT_EQUAL_STRING("1 lines not sent to usb: no reader", console.parsed(2).msg);
    TEST_ASSERT_EQUAL_STRING(console.lines[2], usb.lines[1]);
}

// The report itself is refused (the reader went away again right after): counted again, not
// lost, and reported later.
void test_refused_report_is_counted_again() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);

    class FlakySink : public logcat::Sink {
    public:
        bool write(const char*, size_t) override { return accept[calls++]; }
        const char* name() const override { return "usb"; }
        bool accept[8] = {false, true, false, true, true, true, true, true};
        size_t calls = 0;
    } usb;
    writer.add_sink(usb);

    writer.write(make_record(0, "lost"));           // refused
    writer.write(make_record(1, "ok"));             // accepted: last_ok
    writer.idle();                                  // report "1 lines" refused
    TEST_ASSERT_EQUAL_UINT32(2, writer.unsent(0));  // the lost line + the refused report
    writer.write(make_record(3, "ok again"));
    writer.idle();  // now one report covers both
    TEST_ASSERT_EQUAL_UINT32(0, writer.unsent(0));
    TEST_ASSERT_EQUAL_size_t(5, usb.calls);
}

// Seen as a risk in review: 39 lines refused while COM6 was closed, the port reopens, one line
// fits, the report does not. The next report must cover all 40 seqs, not just the report.
void test_refused_report_keeps_the_count_it_explained() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    FakeSink console("console");
    writer.add_sink(usb);
    writer.add_sink(console);

    usb.refusing = true;
    for (uint32_t i = 0; i < 3; ++i) {
        writer.write(make_record(i, "refused"));
    }
    usb.refusing = false;
    writer.write(make_record(3, "fits"));
    usb.refusing = true;  // the ring is full again when the report comes
    writer.idle();
    TEST_ASSERT_EQUAL_UINT32(4, writer.unsent(0));
    TEST_ASSERT_EQUAL_STRING("3 lines not sent to usb: no reader", console.parsed(4).msg);

    usb.refusing = false;
    writer.write(make_record(5, "back"));
    writer.idle();
    TEST_ASSERT_EQUAL_STRING("4 lines not sent to usb: no reader", usb.parsed(usb.count - 1).msg);
}

// A queue-full report that the only sink refuses: its count joins that sink's unsent lines, so
// the hole the host will see (dropped lines + the report) is fully explained later.
void test_refused_queue_report_joins_the_sink_count() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    writer.add_sink(usb);
    for (int i = 0; i < 5; ++i) {
        sequencer.record_drop();
    }
    usb.refusing = true;
    writer.idle();
    TEST_ASSERT_EQUAL_UINT32(6, writer.unsent(0));
    usb.refusing = false;
    writer.write(make_record(9, "back"));
    writer.idle();
    TEST_ASSERT_EQUAL_STRING("6 lines not sent to usb: no reader", usb.parsed(1).msg);
}

void test_sink_table_is_bounded() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink sinks[logcat::kMaxSinks + 1] = {FakeSink("a"), FakeSink("b"), FakeSink("c"),
                                             FakeSink("d"), FakeSink("e")};
    for (size_t i = 0; i < logcat::kMaxSinks; ++i) {
        TEST_ASSERT_TRUE(writer.add_sink(sinks[i]));
    }
    TEST_ASSERT_FALSE(writer.add_sink(sinks[logcat::kMaxSinks]));
    writer.write(make_record(0, "m"));
    TEST_ASSERT_EQUAL_size_t(0, sinks[logcat::kMaxSinks].calls);
    TEST_ASSERT_EQUAL_UINT32(0, writer.unsent(logcat::kMaxSinks));  // out of range: 0
}

void test_large_counts_are_printed_in_full() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    writer.add_sink(usb);
    for (uint32_t i = 0; i < 1234567; ++i) {
        sequencer.record_drop();
    }
    writer.idle();
    TEST_ASSERT_EQUAL_STRING("dropped 1234567 lines: log queue full", usb.parsed(0).msg);
}

void test_queue_report_comes_before_sink_reports() {
    Sequencer sequencer;
    FakeClock clock;
    Writer writer(sequencer, clock);
    FakeSink usb("usb");
    writer.add_sink(usb);
    usb.refusing = true;
    writer.write(make_record(0, "x"));
    usb.refusing = false;
    writer.write(make_record(1, "y"));
    sequencer.record_drop();
    writer.idle();
    TEST_ASSERT_EQUAL_size_t(3, usb.count);
    TEST_ASSERT_EQUAL_STRING("dropped 1 lines: log queue full", usb.parsed(1).msg);
    TEST_ASSERT_EQUAL_STRING("1 lines not sent to usb: no reader", usb.parsed(2).msg);
    TEST_ASSERT_TRUE(usb.parsed(2).seq == usb.parsed(1).seq + 1);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_write_formats_once_and_reaches_every_sink);
    RUN_TEST(test_idle_without_losses_writes_nothing);
    RUN_TEST(test_queue_drops_are_reported_with_their_own_seq);
    RUN_TEST(test_boot_is_stamped_on_reports);
    RUN_TEST(test_refused_lines_are_counted_and_reported_after_recovery);
    RUN_TEST(test_a_refusing_sink_does_not_affect_the_others);
    RUN_TEST(test_refused_report_is_counted_again);
    RUN_TEST(test_refused_report_keeps_the_count_it_explained);
    RUN_TEST(test_refused_queue_report_joins_the_sink_count);
    RUN_TEST(test_sink_table_is_bounded);
    RUN_TEST(test_large_counts_are_printed_in_full);
    RUN_TEST(test_queue_report_comes_before_sink_reports);
    return UNITY_END();
}
