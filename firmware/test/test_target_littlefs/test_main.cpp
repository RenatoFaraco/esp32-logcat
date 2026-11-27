// The LittleFsFileSystem adapter on the board (#24): the FileSystem port's contract, checked on
// real LittleFS, and the RotatingStore's budget against the real partition.
//
//   pio test -e esp32c3 -f test_target_littlefs --upload-port COMx
//
// WARNING: formats the `logs` partition. The logs kept on the board are erased.
// Only for env:esp32c3 (env:native ignores test_target_*).

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/adapters/littlefs_file_system.cpp"  // the adapter under test (src/ is not built)
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/file_line_reader.hpp"
#include "logcat/line_formatter.hpp"
#include "logcat/rotating_store.hpp"

using logcat::LittleFsFileSystem;
using logcat::MountResult;

namespace {

LittleFsFileSystem fs;

size_t write_text(const char* text) {
    return fs.write(text, std::strlen(text));
}

std::string read_all(const char* name) {
    std::string out;
    char buffer[128];
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

class Count final : public logcat::NameVisitor {
public:
    void visit(const char*) override { ++count; }
    size_t count = 0;
};

void wipe() {
    fs.close();
    TEST_ASSERT_EQUAL(ESP_OK, esp_littlefs_format("logs"));
}

}  // namespace

void setUp() {
    if (fs.mount_result() != MountResult::Failed) {
        wipe();
    }
}
void tearDown() {}

void test_mount() {
    TEST_ASSERT_TRUE(fs.begin() != MountResult::Failed);
    size_t used = 0;
    size_t total = 0;
    TEST_ASSERT_TRUE(fs.usage(used, total));
    printf("littlefs: %u of %u bytes used at mount\n", static_cast<unsigned>(used),
           static_cast<unsigned>(total));
    TEST_ASSERT_EQUAL(512 * 1024, total);
}

void test_append_sync_size_and_read() {
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));
    TEST_ASSERT_EQUAL_size_t(4, write_text("abc\n"));
    TEST_ASSERT_TRUE(fs.sync());
    TEST_ASSERT_EQUAL_INT32(4, fs.size("a.txt"));
    TEST_ASSERT_EQUAL_STRING("abc\n", read_all("a.txt").c_str());
    fs.close();
    TEST_ASSERT_TRUE(fs.open_append("a.txt"));
    write_text("def\n");
    fs.close();
    TEST_ASSERT_EQUAL_STRING("abc\ndef\n", read_all("a.txt").c_str());
}

// The contract's strict side, on the real thing: size() sees only what was synced.
void test_size_of_the_open_file_is_the_synced_size() {
    fs.open_append("a.txt");
    write_text("abc\n");
    fs.sync();
    write_text("unsynced\n");
    TEST_ASSERT_EQUAL_INT32(4, fs.size("a.txt"));
    fs.close();
    TEST_ASSERT_EQUAL_INT32(13, fs.size("a.txt"));
}

void test_the_open_file_is_neither_removed_nor_renamed() {
    fs.open_append("a.txt");
    TEST_ASSERT_FALSE(fs.remove("a.txt"));
    TEST_ASSERT_FALSE(fs.rename("a.txt", "b.txt"));
    fs.close();
    TEST_ASSERT_TRUE(fs.rename("a.txt", "b.txt"));
    TEST_ASSERT_TRUE(fs.remove("b.txt"));
    TEST_ASSERT_EQUAL_INT32(-1, fs.size("b.txt"));
}

// LittleFS replaces a target; the adapter must not.
void test_rename_never_replaces() {
    fs.open_append("a.txt");
    write_text("a");
    fs.open_append("b.txt");
    write_text("b");
    fs.close();
    TEST_ASSERT_FALSE(fs.rename("a.txt", "b.txt"));
    TEST_ASSERT_EQUAL_STRING("b", read_all("b.txt").c_str());
}

void test_list() {
    for (const char* name : {"log_000.txt", "log_001.txt", "x"}) {
        fs.open_append(name);
    }
    fs.close();
    Count count;
    TEST_ASSERT_EQUAL_size_t(3, fs.list(count));
    TEST_ASSERT_EQUAL_size_t(3, count.count);
}

void test_bad_names_and_missing_files() {
    char byte = 0;
    TEST_ASSERT_FALSE(fs.open_append("a/b"));
    TEST_ASSERT_FALSE(fs.open_append(""));
    TEST_ASSERT_EQUAL_INT32(-1, fs.size("none.txt"));
    TEST_ASSERT_EQUAL_size_t(0, fs.read("none.txt", 0, &byte, 1));
    TEST_ASSERT_FALSE(fs.remove("none.txt"));
}

// A full partition: the write fails whole (NOSPC), never short; nothing corrupt afterwards.
void test_a_full_partition() {
    static char block[4096];
    std::memset(block, 'x', sizeof(block));
    TEST_ASSERT_TRUE(fs.open_append("big.txt"));
    size_t total = 0;
    for (int i = 0; i < 200; ++i) {
        const size_t written = fs.write(block, sizeof(block));
        if (written != sizeof(block)) {
            printf("partition full after %u bytes; the failing write took %u\n",
                   static_cast<unsigned>(total), static_cast<unsigned>(written));
            TEST_ASSERT_EQUAL_size_t(0, written);
            break;
        }
        total += written;
    }
    fs.close();
    TEST_ASSERT_TRUE(total > 400 * 1024 && total < 512 * 1024);
}

// The store's 448 KB budget on the real partition: fill it several times over and watch what
// LittleFS itself uses. Criterion 4 is about the store's count; this checks it fits the blocks.
void test_the_store_budget_fits_the_partition() {
    logcat::RotatingStore store(fs);
    TEST_ASSERT_TRUE(store.begin());
    logcat::LogRecord record{};
    record.level = logcat::LogLevel::Info;
    record.set_tag("FIFTEEN_CHARS__");
    std::string message(127, 'm');
    record.set_message(message.c_str());
    char line[logcat::kLineBufferSize];
    size_t peak = 0;
    size_t total = 0;
    const uint32_t lines = 3 * 448 * 1024 / 170;
    for (uint32_t seq = 0; seq < lines; ++seq) {
        record.seq = seq;
        const size_t length = logcat::format_line(record, line);
        TEST_ASSERT_TRUE_MESSAGE(store.write(line, length), "a line was refused");
        if (seq % 64 == 0) {
            store.flush();
            size_t used = 0;
            fs.usage(used, total);
            peak = used > peak ? used : peak;
            vTaskDelay(1);  // let IDLE run: this is a long test
        }
    }
    store.flush();
    printf("store: %u files, %u bytes; littlefs peak %u of %u bytes (%u KB free at the peak)\n",
           static_cast<unsigned>(store.stats().files), static_cast<unsigned>(store.stats().bytes),
           static_cast<unsigned>(peak), static_cast<unsigned>(total),
           static_cast<unsigned>((total - peak) / 1024));
    TEST_ASSERT_TRUE(store.stats().bytes <= 448 * 1024);
    TEST_ASSERT_EQUAL_UINT32(0, store.stats().failed_writes);
}

// What a reset does on LittleFS: the unsynced part of the open file is gone, the file is whole.
void test_the_store_survives_a_remount_and_repairs_a_torn_tail() {
    {
        logcat::RotatingStore store(fs);
        store.begin();
        const char whole[] = "0000:000001 0000000010 I APP: whole\n";
        store.write(whole, sizeof(whole) - 1);
        store.flush();
        // A torn tail as a sync inside a line would leave it, written by hand.
        fs.open_append("log_000.txt");
        write_text("0000:000002 00000");
        fs.sync();
        fs.close();
    }
    // A real unmount and mount, as at the next boot.
    TEST_ASSERT_EQUAL(ESP_OK, esp_vfs_littlefs_unregister("logs"));
    static LittleFsFileSystem remounted;
    TEST_ASSERT_TRUE(remounted.begin() == MountResult::Mounted);
    logcat::RotatingStore again(remounted);
    again.begin();
    TEST_ASSERT_TRUE(again.tail_torn());
    const char next[] = "0000:000003 0000000030 I APP: next\n";
    TEST_ASSERT_TRUE(again.write(next, sizeof(next) - 1));
    again.flush();
    remounted.close();
    logcat::FileLineReader reader(remounted, "log_000.txt");
    logcat::ParseStatus status;
    logcat::LogRecord record{};
    uint32_t statuses[3] = {};
    size_t n = 0;
    while (reader.next(status, record) && n < 3) {
        statuses[n++] = static_cast<uint32_t>(status);
    }
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(logcat::ParseStatus::Ok), statuses[0]);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(logcat::ParseStatus::Torn), statuses[1]);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(logcat::ParseStatus::Ok), statuses[2]);
}

// The dump (#29) reads the files with its own descriptor while the store keeps appending to
// the newest one: neither may disturb the other.
void test_reading_while_a_file_is_open_for_append() {
    fs.open_append("old.txt");
    write_text("old\n");
    fs.close();
    TEST_ASSERT_TRUE(fs.open_append("new.txt"));
    write_text("abc\n");
    TEST_ASSERT_TRUE(fs.sync());
    write_text("more\n");  // not synced: the dump's size stops before it
    char buffer[16] = {};
    TEST_ASSERT_EQUAL_size_t(4, fs.read("old.txt", 0, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY("old\n", buffer, 4);
    TEST_ASSERT_EQUAL_size_t(4, fs.read("new.txt", 0, buffer, 4));  // the open one
    TEST_ASSERT_EQUAL_MEMORY("abc\n", buffer, 4);
    TEST_ASSERT_EQUAL_size_t(4, write_text("end\n"));  // the writer goes on untouched
    TEST_ASSERT_TRUE(fs.sync());
    fs.close();
    TEST_ASSERT_EQUAL_STRING("abc\nmore\nend\n", read_all("new.txt").c_str());
}

// `log clear` (#29) on the real file system: every log file goes, other files stay.
void test_clear_removes_the_log_files_and_starts_log_000() {
    fs.open_append("keep.txt");
    write_text("keep\n");
    fs.close();
    logcat::StoreConfig config;
    config.file_limit = 64;
    logcat::RotatingStore store(fs, config);
    store.begin();
    const char line[] = "0000:000001 0000000010 I APP: 58 bytes, one line per file\n";
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_TRUE(store.write(line, sizeof(line) - 1));
    }
    store.flush();
    TEST_ASSERT_TRUE(store.file_count() > 1);
    TEST_ASSERT_TRUE(store.clear());
    TEST_ASSERT_EQUAL_size_t(1, store.file_count());
    TEST_ASSERT_EQUAL_INT32(0, fs.size("log_000.txt"));
    TEST_ASSERT_EQUAL_INT32(-1, fs.size("log_001.txt"));
    TEST_ASSERT_EQUAL_INT32(5, fs.size("keep.txt"));
    TEST_ASSERT_TRUE(store.write(line, sizeof(line) - 1));
    store.flush();
    fs.close();
    TEST_ASSERT_EQUAL_STRING(line, read_all("log_000.txt").c_str());
}

void test_names_the_port_cannot_carry() {
    char byte = 0;
    TEST_ASSERT_FALSE(fs.open_append("."));
    TEST_ASSERT_FALSE(fs.open_append(".."));
    TEST_ASSERT_FALSE(fs.open_append("a_name_that_is_longer_than_31_chars.txt"));
    fs.open_append("a.txt");
    write_text("abc");
    fs.close();
    TEST_ASSERT_EQUAL_size_t(0, fs.read("a.txt", 10, &byte, 1));  // past the end
}

// What a full partition costs the writer task's stack is measured on the board elsewhere
// (`stats`: writer_stack_free); here, what this test task kept after filling it.
void test_stack_after_a_full_partition() {
    printf("test task stack left: %u B\n",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));  // let the host open the port
    UNITY_BEGIN();
    RUN_TEST(test_mount);
    RUN_TEST(test_append_sync_size_and_read);
    RUN_TEST(test_size_of_the_open_file_is_the_synced_size);
    RUN_TEST(test_the_open_file_is_neither_removed_nor_renamed);
    RUN_TEST(test_rename_never_replaces);
    RUN_TEST(test_list);
    RUN_TEST(test_bad_names_and_missing_files);
    RUN_TEST(test_a_full_partition);
    RUN_TEST(test_the_store_budget_fits_the_partition);
    RUN_TEST(test_reading_while_a_file_is_open_for_append);
    RUN_TEST(test_clear_removes_the_log_files_and_starts_log_000);
    RUN_TEST(test_names_the_port_cannot_carry);
    RUN_TEST(test_stack_after_a_full_partition);
    RUN_TEST(test_the_store_survives_a_remount_and_repairs_a_torn_tail);  // last: it remounts
    UNITY_END();
}
