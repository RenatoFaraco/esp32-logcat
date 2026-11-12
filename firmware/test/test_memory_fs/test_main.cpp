// The in-memory FileSystem fake (#22): the store's tests (#23) and the power-cut tests (M6)
// rely on it behaving like the port promises, so it is tested first.

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
    TEST_ASSERT_EQUAL_INT32(5, fs.size("a.txt"));  // written, even before a sync
    fs.close();
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));
    write(fs, "f");
    TEST_ASSERT_EQUAL_STRING("abcdef", read_all(fs, "a.txt").c_str());
}

void test_read_at_an_offset_and_past_the_end() {
    MemoryFileSystem fs;
    fs.put("a.txt", "0123456789");
    char out[4] = {};
    TEST_ASSERT_EQUAL_size_t(4, fs.read("a.txt", 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("3456", out, 4);
    TEST_ASSERT_EQUAL_size_t(2, fs.read("a.txt", 8, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, fs.read("a.txt", 10, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, fs.read("none.txt", 0, out, sizeof(out)));
}

void test_write_with_nothing_open_writes_nothing() {
    MemoryFileSystem fs;
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "abc"));
    TEST_ASSERT_FALSE(fs.sync());
}

void test_opening_another_file_closes_the_first() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    write(fs, "a");
    fs.open_append("b.txt");
    write(fs, "b");
    TEST_ASSERT_EQUAL_STRING("b.txt", fs.open_file().c_str());
    TEST_ASSERT_EQUAL_STRING("a", fs.content("a.txt").c_str());
    TEST_ASSERT_EQUAL_STRING("b", fs.content("b.txt").c_str());
    fs.power_cut();
    TEST_ASSERT_EQUAL_STRING("a", fs.content("a.txt").c_str());  // closing synced it
}

void test_remove_and_rename() {
    MemoryFileSystem fs;
    fs.put("a.txt", "a");
    fs.put("b.txt", "b");
    TEST_ASSERT_TRUE(fs.rename("a.txt", "c.txt"));
    TEST_ASSERT_FALSE(fs.exists("a.txt"));
    TEST_ASSERT_EQUAL_STRING("a", fs.content("c.txt").c_str());
    TEST_ASSERT_FALSE(fs.rename("c.txt", "b.txt"));  // the target exists
    TEST_ASSERT_FALSE(fs.rename("none.txt", "d.txt"));
    TEST_ASSERT_TRUE(fs.remove("b.txt"));
    TEST_ASSERT_FALSE(fs.remove("b.txt"));
    TEST_ASSERT_EQUAL_size_t(1, fs.file_count());
}

void test_renaming_the_open_file_keeps_it_open() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    TEST_ASSERT_TRUE(fs.rename("a.txt", "b.txt"));
    write(fs, "x");
    TEST_ASSERT_EQUAL_STRING("x", fs.content("b.txt").c_str());
}

void test_removing_the_open_file_closes_it() {
    MemoryFileSystem fs;
    fs.open_append("a.txt");
    TEST_ASSERT_TRUE(fs.remove("a.txt"));
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "x"));
    TEST_ASSERT_FALSE(fs.exists("a.txt"));
}

void test_list_visits_every_file() {
    MemoryFileSystem fs;
    fs.put("log_001.txt", "");
    fs.put("log_000.txt", "x");
    fs.put("other", "y");
    Names names;
    TEST_ASSERT_EQUAL_size_t(3, fs.list(names));
    TEST_ASSERT_EQUAL_size_t(3, names.names.size());
    TEST_ASSERT_TRUE(names.names.count("log_000.txt") == 1);
}

void test_bad_names_are_refused() {
    MemoryFileSystem fs;
    TEST_ASSERT_FALSE(fs.open_append(""));
    TEST_ASSERT_FALSE(fs.open_append(nullptr));
    TEST_ASSERT_FALSE(fs.open_append("dir/file"));
    const std::string longest(logcat::kMaxFileName, 'n');
    TEST_ASSERT_TRUE(fs.open_append(longest.c_str()));
    TEST_ASSERT_FALSE(fs.open_append((longest + "n").c_str()));
    fs.put("a", "");
    TEST_ASSERT_FALSE(fs.rename("a", (longest + "n").c_str()));
}

// ---------------------------------------------------------------- capacity

void test_a_full_partition_gives_short_writes() {
    MemoryFileSystem fs(10);
    fs.open_append("a.txt");
    TEST_ASSERT_EQUAL_size_t(6, write(fs, "123456"));
    TEST_ASSERT_EQUAL_size_t(4, write(fs, "abcdef"));  // only 4 bytes left
    TEST_ASSERT_EQUAL_size_t(0, write(fs, "z"));
    TEST_ASSERT_EQUAL_size_t(10, fs.used());
    fs.remove("a.txt");
    fs.open_append("b.txt");
    TEST_ASSERT_EQUAL_size_t(1, write(fs, "z"));  // freed space is usable again
}

// ---------------------------------------------------------------- failures on demand

void test_failures_happen_once() {
    MemoryFileSystem fs;
    fs.fail_next_open = true;
    TEST_ASSERT_FALSE(fs.open_append("a.txt"));
    TEST_ASSERT_TRUE(fs.open_file().empty());
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));

    fs.fail_next_write = true;
    fs.short_write = 2;
    TEST_ASSERT_EQUAL_size_t(2, write(fs, "abcdef"));
    TEST_ASSERT_EQUAL_size_t(3, write(fs, "ghi"));
    TEST_ASSERT_EQUAL_STRING("abghi", fs.content("a.txt").c_str());

    fs.put("b.txt", "");
    fs.fail_next_rename = true;
    TEST_ASSERT_FALSE(fs.rename("b.txt", "c.txt"));
    TEST_ASSERT_TRUE(fs.rename("b.txt", "c.txt"));

    fs.fail_next_remove = true;
    TEST_ASSERT_FALSE(fs.remove("c.txt"));
    TEST_ASSERT_TRUE(fs.remove("c.txt"));
}

// ---------------------------------------------------------------- power cut

void test_power_cut_loses_what_was_not_synced() {
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

void test_power_cut_can_tear_a_file_anywhere() {
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
    TEST_ASSERT_EQUAL_size_t(1, fs.opens);
    TEST_ASSERT_EQUAL_size_t(2, fs.writes);
    TEST_ASSERT_EQUAL_size_t(5, fs.bytes_written);
    TEST_ASSERT_EQUAL_size_t(1, fs.syncs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_append_creates_and_appends);
    RUN_TEST(test_read_at_an_offset_and_past_the_end);
    RUN_TEST(test_write_with_nothing_open_writes_nothing);
    RUN_TEST(test_opening_another_file_closes_the_first);
    RUN_TEST(test_remove_and_rename);
    RUN_TEST(test_renaming_the_open_file_keeps_it_open);
    RUN_TEST(test_removing_the_open_file_closes_it);
    RUN_TEST(test_list_visits_every_file);
    RUN_TEST(test_bad_names_are_refused);
    RUN_TEST(test_a_full_partition_gives_short_writes);
    RUN_TEST(test_failures_happen_once);
    RUN_TEST(test_power_cut_loses_what_was_not_synced);
    RUN_TEST(test_power_cut_can_tear_a_file_anywhere);
    RUN_TEST(test_power_cut_keeps_files_that_were_closed);
    RUN_TEST(test_a_second_cut_does_not_resurrect_bytes);
    RUN_TEST(test_counters);
    return UNITY_END();
}
