#include <unity.h>

#include <cstdint>

#include "logcat/boot_counter.hpp"

using logcat::BootInfo;
using logcat::BootLoad;
using logcat::next_boot;

namespace {

// The NVS key as a value that may be there or not, with failures on demand.
class FakeBootStore final : public logcat::BootStore {
public:
    BootLoad load(uint32_t& value) override {
        ++loads;
        if (fail_load) {
            value = 12345;  // garbage the counter must not trust
            return BootLoad::Failed;
        }
        if (!present) {
            return BootLoad::Missing;
        }
        value = stored;
        return BootLoad::Found;
    }
    bool save(uint32_t value) override {
        ++saves;
        if (fail_save) {
            return false;
        }
        stored = value;
        present = true;
        return true;
    }

    uint32_t stored = 0;
    bool present = false;
    bool fail_load = false;
    bool fail_save = false;
    int loads = 0;
    int saves = 0;
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_first_boot_is_one() {
    FakeBootStore store;
    const BootInfo info = next_boot(store);
    TEST_ASSERT_EQUAL_UINT32(1, info.boot);
    TEST_ASSERT_TRUE(info.first);
    TEST_ASSERT_TRUE(info.saved);
    TEST_ASSERT_EQUAL_UINT32(1, store.stored);
}

void test_each_boot_adds_one_and_saves_it() {
    FakeBootStore store;
    next_boot(store);
    const BootInfo info = next_boot(store);
    TEST_ASSERT_EQUAL_UINT32(2, info.boot);
    TEST_ASSERT_FALSE(info.first);
    TEST_ASSERT_EQUAL_UINT32(2, store.stored);
    TEST_ASSERT_EQUAL_UINT32(3, next_boot(store).boot);
}

// 0 means "unknown" on the wire: a broken store never yields a number that looks real, and the
// stored value is left alone (it may come back on the next boot).
void test_a_failed_load_is_unknown_and_saves_nothing() {
    FakeBootStore store;
    store.stored = 41;
    store.present = true;
    store.fail_load = true;
    const BootInfo info = next_boot(store);
    TEST_ASSERT_EQUAL_UINT32(logcat::kUnknownBoot, info.boot);
    TEST_ASSERT_FALSE(info.saved);
    TEST_ASSERT_FALSE(info.first);
    TEST_ASSERT_EQUAL(0, store.saves);
    TEST_ASSERT_EQUAL_UINT32(41, store.stored);
}

// The number is still used: the lines of this boot are told apart from the previous one. The
// boot line warns that the next boot may repeat it.
void test_a_failed_save_keeps_the_number_and_says_so() {
    FakeBootStore store;
    store.stored = 7;
    store.present = true;
    store.fail_save = true;
    const BootInfo info = next_boot(store);
    TEST_ASSERT_EQUAL_UINT32(8, info.boot);
    TEST_ASSERT_FALSE(info.saved);
}

void test_the_counter_wraps_to_one_never_to_zero() {
    FakeBootStore store;
    store.stored = UINT32_MAX;
    store.present = true;
    TEST_ASSERT_EQUAL_UINT32(1, next_boot(store).boot);
    TEST_ASSERT_EQUAL_UINT32(1, store.stored);
}

// A stored 0 cannot come from this code; whatever wrote it, the boot is a real one.
void test_a_stored_zero_counts_as_a_real_previous_boot() {
    FakeBootStore store;
    store.present = true;
    store.stored = 0;
    const BootInfo info = next_boot(store);
    TEST_ASSERT_EQUAL_UINT32(1, info.boot);
    TEST_ASSERT_FALSE(info.first);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_first_boot_is_one);
    RUN_TEST(test_each_boot_adds_one_and_saves_it);
    RUN_TEST(test_a_failed_load_is_unknown_and_saves_nothing);
    RUN_TEST(test_a_failed_save_keeps_the_number_and_says_so);
    RUN_TEST(test_the_counter_wraps_to_one_never_to_zero);
    RUN_TEST(test_a_stored_zero_counts_as_a_real_previous_boot);
    return UNITY_END();
}
