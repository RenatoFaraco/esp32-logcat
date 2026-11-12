// RotatingStore and FileLineReader (#23) on the in-memory FileSystem fake (#22): fill, rotate,
// discard the oldest, survive a restart, and never let a torn tail glue to the next record.

#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../support/memory_fs.hpp"
#include "logcat/file_line_reader.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/line_parser.hpp"
#include "logcat/log_record.hpp"
#include "logcat/rotating_store.hpp"

using logcat::FileLineReader;
using logcat::ParseStatus;
using logcat::RotatingStore;
using logcat::StoreConfig;
using test_support::MemoryFileSystem;

namespace {

// A real formatted line: seq numbers make order and loss visible.
std::string make_line(uint32_t seq, const std::string& message = "payload",
                      const char* tag = "APP") {
    logcat::LogRecord record{};
    record.boot = 1;
    record.seq = seq;
    record.uptime_ms = seq * 10;
    record.level = logcat::LogLevel::Info;
    record.set_tag(tag);
    record.set_message(message.c_str());
    char out[logcat::kLineBufferSize];
    const size_t length = logcat::format_line(record, out);
    return std::string(out, length);
}

bool put_line(RotatingStore& store, const std::string& line) {
    return store.write(line.data(), line.size());
}

std::string name_of(uint32_t index) {
    char name[logcat::kStoreNameCapacity];
    logcat::store_file_name(index, name);
    return name;
}

struct Parsed {
    std::vector<ParseStatus> statuses;
    std::vector<uint32_t> seqs;  // of the Ok lines
};

Parsed parse_file(MemoryFileSystem& fs, const std::string& name) {
    Parsed out;
    FileLineReader reader(fs, name.c_str());
    ParseStatus status;
    logcat::LogRecord record{};
    while (reader.next(status, record)) {
        out.statuses.push_back(status);
        if (status == ParseStatus::Ok) {
            out.seqs.push_back(record.seq);
        }
    }
    return out;
}

// Every Ok seq across the files, oldest file first.
std::vector<uint32_t> all_seqs(MemoryFileSystem& fs, const RotatingStore& store) {
    std::vector<uint32_t> seqs;
    for (size_t i = 0; i < store.file_count(); ++i) {
        char name[logcat::kStoreNameCapacity];
        store.file_name(i, name);
        const Parsed parsed = parse_file(fs, name);
        seqs.insert(seqs.end(), parsed.seqs.begin(), parsed.seqs.end());
    }
    return seqs;
}

StoreConfig small(uint32_t file_limit, uint32_t total_limit) {
    StoreConfig config;
    config.file_limit = file_limit;
    config.total_limit = total_limit;
    return config;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- names

void test_file_names() {
    TEST_ASSERT_EQUAL_STRING("log_000.txt", name_of(0).c_str());
    TEST_ASSERT_EQUAL_STRING("log_042.txt", name_of(42).c_str());
    TEST_ASSERT_EQUAL_STRING("log_999.txt", name_of(999).c_str());
}

// ---------------------------------------------------------------- fill, rotate, discard

void test_empty_partition_starts_log_000() {
    MemoryFileSystem fs;
    RotatingStore store(fs);
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_TRUE(put_line(store, make_line(1)));
    store.flush();
    TEST_ASSERT_EQUAL_STRING(make_line(1).c_str(), fs.content("log_000.txt").c_str());
    TEST_ASSERT_EQUAL_STRING("flash", store.name());
}

void test_rotates_when_the_file_is_full() {
    MemoryFileSystem fs;
    const size_t line = make_line(1).size();
    RotatingStore store(fs, small(static_cast<uint32_t>(3 * line), 100 * 1024));
    store.begin();
    for (uint32_t seq = 1; seq <= 7; ++seq) {
        TEST_ASSERT_TRUE(put_line(store, make_line(seq)));
    }
    store.flush();
    TEST_ASSERT_EQUAL_size_t(3, store.file_count());  // 3 + 3 + 1
    TEST_ASSERT_EQUAL_UINT32(2, store.stats().rotations);
    TEST_ASSERT_EQUAL_size_t(3, parse_file(fs, "log_000.txt").seqs.size());
    TEST_ASSERT_EQUAL_size_t(1, parse_file(fs, "log_002.txt").seqs.size());
}

void test_never_passes_the_total_and_keeps_the_newest_in_order() {
    MemoryFileSystem fs;
    RotatingStore store(fs, small(1000, 4000));
    store.begin();
    for (uint32_t seq = 0; seq < 3000; ++seq) {
        TEST_ASSERT_TRUE(put_line(store, make_line(seq)));
        TEST_ASSERT_TRUE(fs.used() <= 4000);  // criterion 4: never, not just at the end
        TEST_ASSERT_TRUE(store.stats().bytes <= 4000);
    }
    store.flush();
    const std::vector<uint32_t> seqs = all_seqs(fs, store);
    TEST_ASSERT_TRUE(seqs.size() > 40);
    TEST_ASSERT_EQUAL_UINT32(2999, seqs.back());
    for (size_t i = 1; i < seqs.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT32(seqs[i - 1] + 1, seqs[i]);  // the newest, contiguous
    }
    TEST_ASSERT_TRUE(store.stats().removed > 0);
}

void test_default_limits_fit_littlefs_blocks() {
    // 512 KB partition, 4 KB blocks, a few metadata blocks: the default 448 KB budget fits.
    MemoryFileSystem fs(512 * 1024, 4096, 8);
    RotatingStore store(fs);
    store.begin();
    const std::string long_line = make_line(0, std::string(127, 'x'));
    for (uint32_t seq = 0; seq < 8000; ++seq) {  // ~1.3 MB written, about 3 times the budget
        TEST_ASSERT_TRUE_MESSAGE(put_line(store, make_line(seq, std::string(127, 'x'))),
                                 "a line was refused: the budget does not fit the partition");
    }
    TEST_ASSERT_TRUE(store.stats().bytes <= 448 * 1024);
    TEST_ASSERT_TRUE(store.file_count() <= 8);
    (void)long_line;
}

// ---------------------------------------------------------------- restart

void test_a_new_store_continues_the_newest_file() {
    MemoryFileSystem fs;
    {
        RotatingStore store(fs, small(1000, 100000));
        store.begin();
        for (uint32_t seq = 0; seq < 30; ++seq) {
            put_line(store, make_line(seq));
        }
        store.flush();
    }
    fs.power_cut();
    RotatingStore again(fs, small(1000, 100000));
    TEST_ASSERT_TRUE(again.begin());
    const size_t files = again.file_count();
    put_line(again, make_line(30));
    again.flush();
    TEST_ASSERT_EQUAL_size_t(files, again.file_count());
    const std::vector<uint32_t> seqs = all_seqs(fs, again);
    TEST_ASSERT_EQUAL_size_t(31, seqs.size());
    TEST_ASSERT_EQUAL_UINT32(30, seqs.back());
    TEST_ASSERT_EQUAL_UINT32(0, again.stats().repaired);  // a clean end needs no repair
}

void test_begin_ignores_other_files_and_removes_beyond_the_limit() {
    MemoryFileSystem fs;
    fs.put("notes.txt", "keep me");
    fs.put("log_12.txt", "not ours");
    fs.put("log_001.txt", std::string(600, 'a') + "\n");
    fs.put("log_002.txt", std::string(600, 'b') + "\n");
    fs.put("log_003.txt", std::string(10, 'c') + "\n");
    RotatingStore store(fs, small(1000, 1000));  // the limit shrank since they were written
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_FALSE(fs.exists("log_001.txt"));
    TEST_ASSERT_TRUE(fs.exists("log_002.txt") && fs.exists("log_003.txt"));
    TEST_ASSERT_TRUE(fs.exists("notes.txt") && fs.exists("log_12.txt"));
    TEST_ASSERT_EQUAL_UINT32(1, store.stats().removed);
}

void test_more_files_than_tracked_are_reduced_to_the_newest() {
    MemoryFileSystem fs;
    for (uint32_t index = 0; index < logcat::kMaxStoreFiles + 4; ++index) {
        fs.put(name_of(index), "x\n");
    }
    RotatingStore store(fs);
    store.begin();
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxStoreFiles, store.file_count());
    TEST_ASSERT_FALSE(fs.exists(name_of(3)));
    TEST_ASSERT_TRUE(fs.exists(name_of(4)));
    char first[logcat::kStoreNameCapacity];
    store.file_name(0, first);
    TEST_ASSERT_EQUAL_STRING(name_of(4).c_str(), first);
}

// ---------------------------------------------------------------- torn tails (the #23 contract)

// The tear can fall anywhere in the line; after the restart the piece is torn and the next
// record is intact, never glued to it.
void test_torn_tail_at_any_offset_is_closed_and_never_glued() {
    const std::string first = make_line(1);
    const std::string cut = make_line(2);
    for (size_t keep = 1; keep < cut.size(); ++keep) {
        MemoryFileSystem fs;
        {
            RotatingStore store(fs);
            store.begin();
            put_line(store, first);
            store.flush();
            put_line(store, cut);
        }
        fs.power_cut(keep);  // `keep` bytes of the second line made it
        RotatingStore again(fs);
        again.begin();
        put_line(again, make_line(3));
        again.flush();
        const Parsed parsed = parse_file(fs, "log_000.txt");
        TEST_ASSERT_EQUAL_size_t_MESSAGE(3, parsed.statuses.size(), cut.substr(0, keep).c_str());
        TEST_ASSERT_TRUE(parsed.statuses[0] == ParseStatus::Ok);
        TEST_ASSERT_TRUE_MESSAGE(parsed.statuses[1] == ParseStatus::Torn,
                                 cut.substr(0, keep).c_str());
        TEST_ASSERT_TRUE(parsed.statuses[2] == ParseStatus::Ok);
        TEST_ASSERT_EQUAL_UINT32(3, parsed.seqs.back());
        TEST_ASSERT_EQUAL_UINT32(1, again.stats().repaired);
    }
}

// What LittleFS really produces: a sync inside a line (here: a short write, then the close).
void test_a_short_write_is_repaired_before_the_next_line() {
    MemoryFileSystem fs;
    RotatingStore store(fs);
    store.begin();
    put_line(store, make_line(1));
    store.flush();
    fs.fail_next_write = true;
    fs.short_write = 10;
    TEST_ASSERT_FALSE(put_line(store, make_line(2)));
    TEST_ASSERT_EQUAL_UINT32(1, store.stats().failed_writes);
    TEST_ASSERT_TRUE(put_line(store, make_line(3)));
    store.flush();
    const Parsed parsed = parse_file(fs, "log_000.txt");
    TEST_ASSERT_EQUAL_size_t(2, parsed.seqs.size());  // 1 and 3; the 10 bytes of 2 were lost
    TEST_ASSERT_EQUAL_UINT32(3, parsed.seqs.back());
    for (const ParseStatus status : parsed.statuses) {
        TEST_ASSERT_TRUE(status == ParseStatus::Ok || status == ParseStatus::Torn);
    }
}

void test_a_cut_right_after_a_newline_needs_no_repair() {
    MemoryFileSystem fs;
    fs.put("log_000.txt", make_line(1));
    RotatingStore store(fs);
    store.begin();
    put_line(store, make_line(2));
    store.flush();
    TEST_ASSERT_EQUAL_UINT32(0, store.stats().repaired);
    TEST_ASSERT_EQUAL_size_t(2, parse_file(fs, "log_000.txt").seqs.size());
}

void test_one_write_per_line() {
    MemoryFileSystem fs;
    RotatingStore store(fs);
    store.begin();
    const size_t before = fs.writes;
    put_line(store, make_line(1));
    put_line(store, make_line(2));
    TEST_ASSERT_EQUAL_size_t(before + 2, fs.writes);
}

// ---------------------------------------------------------------- failures

void test_a_full_partition_refuses_lines_and_recovers() {
    MemoryFileSystem fs(300);
    RotatingStore store(fs, small(10000, 10000));  // the budget is wrong for this partition
    store.begin();
    size_t refused = 0;
    for (uint32_t seq = 0; seq < 20; ++seq) {
        refused += put_line(store, make_line(seq)) ? 0 : 1;
    }
    TEST_ASSERT_TRUE(refused > 0);
    fs.set_capacity(100000);
    TEST_ASSERT_TRUE(put_line(store, make_line(99)));
    store.flush();
    const Parsed parsed = parse_file(fs, "log_000.txt");
    TEST_ASSERT_EQUAL_UINT32(99, parsed.seqs.back());
}

void test_an_oldest_file_that_cannot_be_removed_refuses_the_line() {
    MemoryFileSystem fs;
    const size_t line = make_line(1).size();
    RotatingStore store(fs, small(static_cast<uint32_t>(line), static_cast<uint32_t>(2 * line)));
    store.begin();
    put_line(store, make_line(1));
    put_line(store, make_line(2));
    fs.fail_next_remove = true;
    TEST_ASSERT_FALSE(put_line(store, make_line(3)));  // over the limit otherwise: refused
    TEST_ASSERT_EQUAL_UINT32(1, store.stats().failed_removes);
    TEST_ASSERT_TRUE(store.stats().bytes <= 2 * line);
    TEST_ASSERT_TRUE(put_line(store, make_line(4)));  // the next try removes it
}

void test_an_open_that_fails_is_retried_at_the_next_line() {
    MemoryFileSystem fs;
    RotatingStore store(fs);
    fs.fail_next_open = true;
    TEST_ASSERT_FALSE(store.begin());
    TEST_ASSERT_TRUE(put_line(store, make_line(1)));
    store.flush();
    TEST_ASSERT_EQUAL_size_t(1, parse_file(fs, "log_000.txt").seqs.size());
}

void test_flush_reports_a_failed_sync() {
    MemoryFileSystem fs;
    RotatingStore store(fs);
    store.begin();
    put_line(store, make_line(1));
    fs.fail_next_sync = true;
    TEST_ASSERT_FALSE(store.flush());
    TEST_ASSERT_TRUE(store.flush());
}

// ---------------------------------------------------------------- after log_999

void test_after_999_the_files_are_renumbered_in_order() {
    MemoryFileSystem fs;
    fs.put(name_of(997), make_line(1));
    fs.put(name_of(998), make_line(2));
    fs.put(name_of(999), make_line(3));
    const size_t line = make_line(1).size();
    RotatingStore store(fs, small(static_cast<uint32_t>(line), 100000));
    store.begin();
    TEST_ASSERT_TRUE(put_line(store, make_line(4)));  // log_999 is full: rotate past 999
    store.flush();
    TEST_ASSERT_EQUAL_UINT32(1, store.stats().renumbered);
    TEST_ASSERT_FALSE(fs.exists(name_of(999)));
    const std::vector<uint32_t> seqs = all_seqs(fs, store);
    TEST_ASSERT_EQUAL_size_t(4, seqs.size());
    for (uint32_t i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i + 1, seqs[i]);
    }
    TEST_ASSERT_TRUE(fs.exists(name_of(3)));  // the new current file
}

// A cut in the middle of the renumbering leaves the oldest renamed low and the newest high:
// the order by index is still the order of the files.
void test_a_renumbering_cut_halfway_keeps_the_order() {
    MemoryFileSystem fs;
    fs.put(name_of(0), make_line(1));
    fs.put(name_of(998), make_line(2));
    fs.put(name_of(999), make_line(3));
    const size_t line = make_line(1).size();
    RotatingStore store(fs, small(static_cast<uint32_t>(line), 100000));
    store.begin();
    TEST_ASSERT_TRUE(put_line(store, make_line(4)));  // rotates past 999: renumbers
    TEST_ASSERT_EQUAL_UINT32(1, store.stats().renumbered);
    TEST_ASSERT_TRUE(fs.exists(name_of(0)) && fs.exists(name_of(1)) && fs.exists(name_of(2)));
    store.flush();
    const std::vector<uint32_t> seqs = all_seqs(fs, store);
    TEST_ASSERT_EQUAL_size_t(4, seqs.size());
    TEST_ASSERT_EQUAL_UINT32(1, seqs.front());
    TEST_ASSERT_EQUAL_UINT32(4, seqs.back());
}

void test_a_rename_that_fails_refuses_the_line() {
    MemoryFileSystem fs;
    fs.put(name_of(998), make_line(1));
    fs.put(name_of(999), make_line(2));
    const size_t line = make_line(1).size();
    RotatingStore store(fs, small(static_cast<uint32_t>(line), 100000));
    store.begin();
    fs.fail_next_rename = true;
    TEST_ASSERT_FALSE(put_line(store, make_line(3)));
    TEST_ASSERT_TRUE(put_line(store, make_line(4)));
    store.flush();
    TEST_ASSERT_EQUAL_UINT32(4, all_seqs(fs, store).back());
}

// ---------------------------------------------------------------- the line reader

void test_reader_counts_each_kind_of_line() {
    MemoryFileSystem fs;
    const std::string overlong(300, 'z');
    fs.put("f.txt", make_line(1) + "\xff\xff\xff\n" + overlong + "\n" + make_line(2) +
                        std::string("bad header line\n") + "0000:000003 0000");
    FileLineReader reader(fs, "f.txt");
    ParseStatus status;
    logcat::LogRecord record{};
    std::vector<ParseStatus> got;
    while (reader.next(status, record)) {
        got.push_back(status);
    }
    TEST_ASSERT_EQUAL_size_t(6, got.size());
    TEST_ASSERT_TRUE(got[0] == ParseStatus::Ok);
    TEST_ASSERT_TRUE(got[1] == ParseStatus::Garbage);
    TEST_ASSERT_TRUE(got[2] == ParseStatus::Overlong);  // the rest of it is skipped, not a line
    TEST_ASSERT_TRUE(got[3] == ParseStatus::Ok);
    TEST_ASSERT_TRUE(got[4] == ParseStatus::BadHeader);
    TEST_ASSERT_TRUE(got[5] == ParseStatus::Torn);  // last piece, no '\n'
    TEST_ASSERT_EQUAL_UINT32(2, reader.count(ParseStatus::Ok));
    TEST_ASSERT_EQUAL_UINT32(1, reader.count(ParseStatus::Torn));
}

void test_reader_line_of_exactly_170_bytes_and_a_closed_torn_tail_of_171() {
    MemoryFileSystem fs;
    const std::string full = make_line(1, std::string(127, 'm'), "FIFTEEN_CHARS__");
    TEST_ASSERT_EQUAL_size_t(170, full.size());
    const std::string torn = full.substr(0, 169) + "\x7f\n";  // 171 bytes, closed by the store
    fs.put("f.txt", full + torn + make_line(2));
    const Parsed parsed = parse_file(fs, "f.txt");
    TEST_ASSERT_EQUAL_size_t(3, parsed.statuses.size());
    TEST_ASSERT_TRUE(parsed.statuses[0] == ParseStatus::Ok);
    TEST_ASSERT_TRUE(parsed.statuses[1] == ParseStatus::Torn);
    TEST_ASSERT_TRUE(parsed.statuses[2] == ParseStatus::Ok);
}

void test_reader_erased_flash_without_newlines_is_one_overlong_line() {
    MemoryFileSystem fs;
    fs.put("f.txt", std::string(2000, '\xff') + "\n" + make_line(7));
    const Parsed parsed = parse_file(fs, "f.txt");
    TEST_ASSERT_EQUAL_size_t(2, parsed.statuses.size());
    TEST_ASSERT_TRUE(parsed.statuses[0] == ParseStatus::Overlong);
    TEST_ASSERT_EQUAL_UINT32(7, parsed.seqs.back());
}

void test_reader_nul_bytes_and_lines_across_blocks() {
    MemoryFileSystem fs;
    std::string content;
    for (uint32_t seq = 0; seq < 40; ++seq) {  // ~2 KB: several 512-byte blocks
        content += make_line(seq);
    }
    content += std::string("a\0b\n", 4);
    fs.put("f.txt", content);
    const Parsed parsed = parse_file(fs, "f.txt");
    TEST_ASSERT_EQUAL_size_t(40, parsed.seqs.size());
    TEST_ASSERT_TRUE(parsed.statuses.back() == ParseStatus::Garbage);
}

void test_reader_of_an_empty_or_missing_file() {
    MemoryFileSystem fs;
    fs.put("empty.txt", "");
    TEST_ASSERT_EQUAL_size_t(0, parse_file(fs, "empty.txt").statuses.size());
    TEST_ASSERT_EQUAL_size_t(0, parse_file(fs, "none.txt").statuses.size());
}

void test_reader_stops_at_the_size_taken_at_the_start() {
    MemoryFileSystem fs;
    fs.put("f.txt", make_line(1));
    FileLineReader reader(fs, "f.txt");
    fs.put("f.txt", make_line(1) + make_line(2));  // grows while reading
    ParseStatus status;
    logcat::LogRecord record{};
    TEST_ASSERT_TRUE(reader.next(status, record));
    TEST_ASSERT_FALSE(reader.next(status, record));
    TEST_ASSERT_EQUAL_UINT32(make_line(1).size(), reader.file_size());
}

void test_an_unreadable_last_byte_is_repaired_rather_than_trusted() {
    MemoryFileSystem fs;
    fs.put("log_000.txt", make_line(1));
    fs.fail_next_read = true;
    RotatingStore store(fs);
    store.begin();
    put_line(store, make_line(2));
    store.flush();
    TEST_ASSERT_EQUAL_UINT32(1, store.stats().repaired);
}

void test_rotation_at_the_exact_file_limit() {
    MemoryFileSystem fs;
    const size_t line = make_line(1).size();
    RotatingStore store(fs, small(static_cast<uint32_t>(2 * line - 1), 100000));
    store.begin();
    put_line(store, make_line(1));
    put_line(store, make_line(2));  // 2 lines are one byte too many: a new file
    TEST_ASSERT_EQUAL_size_t(2, store.file_count());
    MemoryFileSystem other;
    RotatingStore exact(other, small(static_cast<uint32_t>(2 * line), 100000));
    exact.begin();
    put_line(exact, make_line(1));
    put_line(exact, make_line(2));  // exactly the limit: same file
    TEST_ASSERT_EQUAL_size_t(1, exact.file_count());
}

void test_a_line_longer_than_the_file_limit_goes_into_an_empty_file() {
    MemoryFileSystem fs;
    RotatingStore store(fs, small(10, 100000));
    store.begin();
    TEST_ASSERT_TRUE(put_line(store, make_line(1)));
    TEST_ASSERT_EQUAL_size_t(1, store.file_count());  // no empty file left behind
    TEST_ASSERT_TRUE(put_line(store, make_line(2)));
    TEST_ASSERT_EQUAL_size_t(2, store.file_count());
}

void test_a_line_over_the_total_with_one_file_never_tries_to_remove_it() {
    MemoryFileSystem fs;
    RotatingStore store(fs, small(100000, 10));
    store.begin();
    TEST_ASSERT_FALSE(put_line(store, make_line(1)));
    TEST_ASSERT_EQUAL_UINT32(0, store.stats().failed_removes);
}

void test_the_repair_counts_toward_the_total() {
    MemoryFileSystem fs;
    const std::string line = make_line(1);
    fs.put("log_000.txt", line);
    fs.put("log_001.txt", "torn");  // the current file, cut inside a line
    // Room for the line but not for the line plus DEL + '\n': the oldest must go.
    const uint32_t limit = static_cast<uint32_t>(line.size() + 4 + line.size() + 1);
    RotatingStore store(fs, small(100000, limit));
    store.begin();
    TEST_ASSERT_TRUE(put_line(store, make_line(2)));
    TEST_ASSERT_FALSE(fs.exists("log_000.txt"));
    TEST_ASSERT_TRUE(store.stats().bytes <= limit);
}

void test_at_most_16_files_are_kept_even_when_the_total_allows_more() {
    MemoryFileSystem fs;
    const size_t line = make_line(1).size();
    RotatingStore store(fs, small(static_cast<uint32_t>(line), 1000000));
    store.begin();
    for (uint32_t seq = 0; seq < logcat::kMaxStoreFiles + 5; ++seq) {
        TEST_ASSERT_TRUE(put_line(store, make_line(seq)));
    }
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxStoreFiles, store.file_count());
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxStoreFiles, fs.file_count());
}

void test_only_log_nnn_txt_names_are_ours() {
    MemoryFileSystem fs;
    for (const char* name :
         {"log_001.bin", "log_001.txtx", "LOG_001.txt", "log_0a1.txt", "xlog_001.txt"}) {
        fs.put(name, "not ours");
    }
    RotatingStore store(fs);
    store.begin();
    TEST_ASSERT_EQUAL_size_t(1, store.file_count());  // only the new log_000.txt
    char first[logcat::kStoreNameCapacity];
    store.file_name(0, first);
    TEST_ASSERT_EQUAL_STRING("log_000.txt", first);
    TEST_ASSERT_FALSE(store.file_name(1, first));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_an_unreadable_last_byte_is_repaired_rather_than_trusted);
    RUN_TEST(test_rotation_at_the_exact_file_limit);
    RUN_TEST(test_a_line_longer_than_the_file_limit_goes_into_an_empty_file);
    RUN_TEST(test_a_line_over_the_total_with_one_file_never_tries_to_remove_it);
    RUN_TEST(test_the_repair_counts_toward_the_total);
    RUN_TEST(test_at_most_16_files_are_kept_even_when_the_total_allows_more);
    RUN_TEST(test_only_log_nnn_txt_names_are_ours);
    RUN_TEST(test_file_names);
    RUN_TEST(test_empty_partition_starts_log_000);
    RUN_TEST(test_rotates_when_the_file_is_full);
    RUN_TEST(test_never_passes_the_total_and_keeps_the_newest_in_order);
    RUN_TEST(test_default_limits_fit_littlefs_blocks);
    RUN_TEST(test_a_new_store_continues_the_newest_file);
    RUN_TEST(test_begin_ignores_other_files_and_removes_beyond_the_limit);
    RUN_TEST(test_more_files_than_tracked_are_reduced_to_the_newest);
    RUN_TEST(test_torn_tail_at_any_offset_is_closed_and_never_glued);
    RUN_TEST(test_a_short_write_is_repaired_before_the_next_line);
    RUN_TEST(test_a_cut_right_after_a_newline_needs_no_repair);
    RUN_TEST(test_one_write_per_line);
    RUN_TEST(test_a_full_partition_refuses_lines_and_recovers);
    RUN_TEST(test_an_oldest_file_that_cannot_be_removed_refuses_the_line);
    RUN_TEST(test_an_open_that_fails_is_retried_at_the_next_line);
    RUN_TEST(test_flush_reports_a_failed_sync);
    RUN_TEST(test_after_999_the_files_are_renumbered_in_order);
    RUN_TEST(test_a_renumbering_cut_halfway_keeps_the_order);
    RUN_TEST(test_a_rename_that_fails_refuses_the_line);
    RUN_TEST(test_reader_counts_each_kind_of_line);
    RUN_TEST(test_reader_line_of_exactly_170_bytes_and_a_closed_torn_tail_of_171);
    RUN_TEST(test_reader_erased_flash_without_newlines_is_one_overlong_line);
    RUN_TEST(test_reader_nul_bytes_and_lines_across_blocks);
    RUN_TEST(test_reader_of_an_empty_or_missing_file);
    RUN_TEST(test_reader_stops_at_the_size_taken_at_the_start);
    return UNITY_END();
}
