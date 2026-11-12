// The in-memory FileSystem fake (#22): the store's tests (#23) and the power-cut tests (M6)
// rely on it keeping the port's contract (logcat/file_system.hpp), so it is tested first.

#include <unity.h>

#include <set>
#include <string>

#include "../support/memory_fs.hpp"
#include "logcat/file_system.hpp"

using test_support::MemoryFileSystem;

namespace {

class Names final : public logcat::NameVisitor {
public:
    void visit(const char* name) override { names.insert(name); }
    std::set<std::string> names;
};

size_t write(MemoryFileSystem& fs, const std::string& text) {
    return fs.write(text.data(), text.size());
}

std::string read_all(MemoryFileSystem& fs, const char* name) {
    char buffer[256];
    std::string out;
    uint32_t offset = 0;
    for (;;) {
        const size_t got = fs.read(name, offset, buffer, sizeof(buffer));
        if (got == 0) {
            return out;
        }
        out.append(buffer, got);
        offset += static_cast<uint32_t>(got);
    }
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- basic operations

void test_append_creates_and_appends() {
    MemoryFileSystem fs;
    TEST_ASSERT_EQUAL_INT32(-1, fs.size("a.txt"));
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));
    TEST_ASSERT_EQUAL_INT32(0, fs.size("a.txt"));
    TEST_ASSERT_EQUAL_size_t(3, write(fs, "abc"));
    TEST_ASSERT_EQUAL_size_t(2, write(fs, "de"));
    fs.close();
    TEST_ASSERT_EQUAL_INT32(5, fs.size("a.txt"));
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));
    write(fs, "f");
    fs.close();
    TEST_ASSERT_EQUAL_STRING("abcdef", read_all(fs, "a.txt").c_str());
}

// The strict side of LittleFS: a size or a read of the open file shows only what was synced.
void test_size_and_read_of_the_open_file_show_only_synced_bytes() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "abc");
    TEST_ASSERT_EQUAL_INT32(0, fs.size("a.txt"));
    TEST_ASSERT_EQUAL_STRING("", read_all(fs, "a.txt").c_str());
    TEST_ASSERT_TRUE(fs.sync());
    write(fs, "de");
    TEST_ASSERT_EQUAL_INT32(3, fs.size("a.txt"));
    TEST_ASSERT_EQUAL_STRING("abc", read_all(fs, "a.txt").c_str());
}

void test_read_at_an_offset_and_past_the_end() {
    MemoryFileSystem fs;
    fs.put("a.txt", "0123456789");
    char out[4] = {};
    TEST_ASSERT_EQUAL_size_t(4, fs.read("a.txt", 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("3456", out, 4);
    TEST_ASSERT_EQUAL_size_t(2, fs.read("a.txt", 8, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, fs.read("a.txt", 10, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, fs.read("a.txt", 0, out, 0));
    TEST_ASSERT_EQUAL_size_t(0, fs.read("none.txt", 0, out, sizeof(out)));
}

void test_write_with_nothing_open_writes_nothing() {
    MemoryFileSystem fs;
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "abc"));
    TEST_ASSERT_FALSE(fs.sync());
}

void test_opening_another_file_closes_and_syncs_the_first() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "a");
    fs.open_append("b.txt");
    write(fs, "b");
    TEST_ASSERT_EQUAL_STRING("b.txt", fs.open_file().c_str());
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("a", fs.content("a.txt").c_str());
    TEST_ASSERT_EQUAL_STRING("", fs.content("b.txt").c_str());
}

void test_reopening_the_open_file_keeps_its_data() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "a");
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));
    write(fs, "b");
    fs.close();
    TEST_ASSERT_EQUAL_STRING("ab", fs.content("a.txt").c_str());
}

void test_a_failed_open_leaves_nothing_open_and_the_previous_file_synced() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "a");
    fs.fail_next_open = true;
    TEST_ASSERT_FALSE(fs.open_append("b.txt"));
    TEST_ASSERT_TRUE(fs.open_file().empty());
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("a", fs.content("a.txt").c_str());
}

void test_remove_and_rename() {
    MemoryFileSystem fs;
    fs.put("a.txt", "a");
    fs.put("b.txt", "b");
    TEST_ASSERT_TRUE(fs.rename("a.txt", "c.txt"));
    TEST_ASSERT_FALSE(fs.exists("a.txt"));
    TEST_ASSERT_EQUAL_STRING("a", fs.content("c.txt").c_str());
    TEST_ASSERT_FALSE(fs.rename("c.txt", "b.txt"));  // the target exists: never replaced
    TEST_ASSERT_EQUAL_STRING("b", fs.content("b.txt").c_str());
    TEST_ASSERT_FALSE(fs.rename("none.txt", "d.txt"));
    TEST_ASSERT_TRUE(fs.remove("b.txt"));
    TEST_ASSERT_FALSE(fs.remove("b.txt"));
    TEST_ASSERT_EQUAL_size_t(1, fs.file_count());
}

void test_the_open_file_can_be_neither_removed_nor_renamed() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    TEST_ASSERT_FALSE(fs.remove("a.txt"));
    TEST_ASSERT_FALSE(fs.rename("a.txt", "b.txt"));
    fs.close();
    TEST_ASSERT_TRUE(fs.rename("a.txt", "b.txt"));
    TEST_ASSERT_TRUE(fs.remove("b.txt"));
}

void test_a_renamed_file_keeps_what_was_synced() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "a");
    fs.close();
    TEST_ASSERT_TRUE(fs.rename("a.txt", "b.txt"));
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("a", fs.content("b.txt").c_str());
}

void test_list_visits_every_file() {
    MemoryFileSystem fs;
    Names empty;
    TEST_ASSERT_EQUAL_size_t(0, fs.list(empty));
    fs.put("log_001.txt", "");
    fs.put("log_000.txt", "x");
    fs.put("other", "y");
    Names names;
    TEST_ASSERT_EQUAL_size_t(3, fs.list(names));
    TEST_ASSERT_EQUAL_size_t(3, names.names.size());
    TEST_ASSERT_TRUE(names.names.count("log_000.txt") == 1);
}

void test_bad_names_are_refused_everywhere() {
    MemoryFileSystem fs;
    TEST_ASSERT_FALSE(fs.open_append(""));
    TEST_ASSERT_FALSE(fs.open_append(nullptr));
    TEST_ASSERT_FALSE(fs.open_append("dir/file"));
    const std::string longest(logcat::kMaxFileName, 'n');
    TEST_ASSERT_TRUE(fs.open_append(longest.c_str()));
    TEST_ASSERT_FALSE(fs.open_append((longest + "n").c_str()));
    fs.put("a", "");
    TEST_ASSERT_FALSE(fs.rename("a", (longest + "n").c_str()));
    char out[1];
    TEST_ASSERT_EQUAL_INT32(-1, fs.size(nullptr));
    TEST_ASSERT_EQUAL_size_t(0, fs.read(nullptr, 0, out, 1));
    TEST_ASSERT_FALSE(fs.remove(nullptr));
    TEST_ASSERT_FALSE(fs.rename(nullptr, "b"));
    TEST_ASSERT_FALSE(fs.rename("a", nullptr));
}

// ---------------------------------------------------------------- capacity

void test_a_full_partition_gives_short_writes() {
    MemoryFileSystem fs(10);
    fs.open_append("a.txt");
    TEST_ASSERT_EQUAL_size_t(6, write(fs, "123456"));
    TEST_ASSERT_EQUAL_size_t(4, write(fs, "abcdef"));  // only 4 bytes left
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "z"));
    TEST_ASSERT_EQUAL_size_t(10, fs.used());
    fs.close();
    fs.remove("a.txt");
    fs.open_append("b.txt");
    TEST_ASSERT_EQUAL_size_t(1, write(fs, "z"));  // freed space is usable again
}

void test_capacity_lowered_below_what_is_used() {
    MemoryFileSystem fs;
    fs.put("a.txt", "0123456789");
    fs.set_capacity(5);
    fs.open_append("b.txt");
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "x"));
}

void test_blocks_like_littlefs() {
    MemoryFileSystem fs(4 * 4096, 4096, 1);  // four blocks, one of them metadata
    fs.put("a.txt", "x");                    // one byte still takes a whole block
    TEST_ASSERT_EQUAL_size_t(2 * 4096, fs.used());
    fs.open_append("b.txt");
    const std::string block(4096, 'b');
    TEST_ASSERT_EQUAL_size_t(4096, write(fs, block));
    TEST_ASSERT_EQUAL_size_t(4096, write(fs, block));
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "c"));  // a.txt, b.txt (2), metadata: full
    TEST_ASSERT_EQUAL_size_t(4 * 4096, fs.used());
}

// ---------------------------------------------------------------- a short write puts the file in
// error

void test_after_a_short_write_sync_fails_and_close_keeps_only_the_last_sync() {
    MemoryFileSystem fs(12);
    fs.open_append("a.txt");
    write(fs, "kept\n");
    TEST_ASSERT_TRUE(fs.sync());
    TEST_ASSERT_EQUAL_size_t(7, write(fs, "lost and cut\n"));
    TEST_ASSERT_FALSE(fs.sync());
    fs.close();
    TEST_ASSERT_EQUAL_STRING("kept\n", fs.content("a.txt").c_str());
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));  // reopening clears the error
    TEST_ASSERT_EQUAL_size_t(5, write(fs, "more\n"));
    TEST_ASSERT_TRUE(fs.sync());
}

// ---------------------------------------------------------------- failures on demand

void test_failures_happen_once() {
    MemoryFileSystem fs;
    fs.fail_next_open = true;
    TEST_ASSERT_FALSE(fs.open_append("a.txt"));
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));

    fs.fail_next_write = true;
    fs.short_write = 2;
    TEST_ASSERT_EQUAL_size_t(2, write(fs, "abcdef"));
    fs.close();  // in error: reopen
    fs.open_append("a.txt");
    TEST_ASSERT_EQUAL_size_t(3, write(fs, "ghi"));

    fs.fail_next_sync = true;
    TEST_ASSERT_FALSE(fs.sync());
    TEST_ASSERT_TRUE(fs.sync());
    TEST_ASSERT_EQUAL_STRING("ghi", fs.content("a.txt").c_str());

    fs.put("b.txt", "");
    fs.fail_next_rename = true;
    TEST_ASSERT_FALSE(fs.rename("b.txt", "c.txt"));
    TEST_ASSERT_TRUE(fs.rename("b.txt", "c.txt"));

    fs.fail_next_remove = true;
    TEST_ASSERT_FALSE(fs.remove("c.txt"));
    TEST_ASSERT_TRUE(fs.remove("c.txt"));
}

void test_a_failed_sync_keeps_the_data_pending() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "a\n");
    fs.fail_next_sync = true;
    TEST_ASSERT_FALSE(fs.sync());
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("", fs.content("a.txt").c_str());
}

void test_a_write_failure_armed_with_nothing_open_is_used_up() {
    MemoryFileSystem fs;
    fs.fail_next_write = true;
    write(fs, "x");
    fs.open_append("a.txt");
    TEST_ASSERT_EQUAL_size_t(3, write(fs, "abc"));
}

// ---------------------------------------------------------------- power cut

void test_power_cut_rolls_back_to_the_last_sync() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "kept\n");
    TEST_ASSERT_TRUE(fs.sync());
    write(fs, "lost\n");
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("kept\n", fs.content("a.txt").c_str());
    TEST_ASSERT_TRUE(fs.open_file().empty());
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "x"));  // nothing open after the cut
}

// How a torn last line really happens on LittleFS: a sync after part of a line.
void test_a_sync_inside_a_line_leaves_it_torn_after_a_cut() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "kept\npart");
    fs.sync();
    write(fs, " of it\n");
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("kept\npart", fs.content("a.txt").c_str());
}

void test_power_cut_with_extra_bytes_can_tear_anywhere() {
    for (size_t extra = 0; extra <= 6; ++extra) {
        MemoryFileSystem fs;
        fs.open_append("a.txt");
        write(fs, "kept\n");
        fs.sync();
        write(fs, "torn\n");
        fs.power_cut(extra);
        const std::string expected = std::string("kept\n") + std::string("torn\n").substr(0, extra);
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), fs.content("a.txt").c_str());
    }
}

void test_power_cut_keeps_files_that_were_closed() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "a\n");
    fs.close();
    fs.open_append("b.txt");
    write(fs, "b\n");
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("a\n", fs.content("a.txt").c_str());
    TEST_ASSERT_EQUAL_STRING("", fs.content("b.txt").c_str());
    TEST_ASSERT_TRUE(fs.exists("b.txt"));  // created, but empty
}

void test_a_second_cut_does_not_resurrect_bytes() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "abcdef");
    fs.power_cut(2);
    fs.power_cut(10);
    TEST_ASSERT_EQUAL_STRING("ab", fs.content("a.txt").c_str());
}

void test_counters() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "abc");
    write(fs, "de");
    fs.sync();
    fs.close();
    TEST_ASSERT_EQUAL_size_t(1, fs.opens);
    TEST_ASSERT_EQUAL_size_t(2, fs.writes);
    TEST_ASSERT_EQUAL_size_t(5, fs.bytes_written);
    TEST_ASSERT_EQUAL_size_t(1, fs.syncs);  // the close is counted apart
    TEST_ASSERT_EQUAL_size_t(1, fs.closes);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_append_creates_and_appends);
    RUN_TEST(test_size_and_read_of_the_open_file_show_only_synced_bytes);
    RUN_TEST(test_read_at_an_offset_and_past_the_end);
    RUN_TEST(test_write_with_nothing_open_writes_nothing);
    RUN_TEST(test_opening_another_file_closes_and_syncs_the_first);
    RUN_TEST(test_reopening_the_open_file_keeps_its_data);
    RUN_TEST(test_a_failed_open_leaves_nothing_open_and_the_previous_file_synced);
    RUN_TEST(test_remove_and_rename);
    RUN_TEST(test_the_open_file_can_be_neither_removed_nor_renamed);
    RUN_TEST(test_a_renamed_file_keeps_what_was_synced);
    RUN_TEST(test_list_visits_every_file);
    RUN_TEST(test_bad_names_are_refused_everywhere);
    RUN_TEST(test_a_full_partition_gives_short_writes);
    RUN_TEST(test_capacity_lowered_below_what_is_used);
    RUN_TEST(test_blocks_like_littlefs);
    RUN_TEST(test_after_a_short_write_sync_fails_and_close_keeps_only_the_last_sync);
    RUN_TEST(test_failures_happen_once);
    RUN_TEST(test_a_failed_sync_keeps_the_data_pending);
    RUN_TEST(test_a_write_failure_armed_with_nothing_open_is_used_up);
    RUN_TEST(test_power_cut_rolls_back_to_the_last_sync);
    RUN_TEST(test_a_sync_inside_a_line_leaves_it_torn_after_a_cut);
    RUN_TEST(test_power_cut_with_extra_bytes_can_tear_anywhere);
    RUN_TEST(test_power_cut_keeps_files_that_were_closed);
    RUN_TEST(test_a_second_cut_does_not_resurrect_bytes);
    RUN_TEST(test_counters);
    return UNITY_END();
}
