// The NvsBootStore adapter on the board (#25), in its own NVS namespace (`logcat_test`): the
// real boot counter (`logcat`) is not touched.
//
// One suite of the on-board test app (#93, test_main.cpp runs it).

#include <unity.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/boot_counter.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "port/nvs_boot_store.hpp"  // the adapter under test
#include "suites.hpp"

using logcat::BootLoad;
using logcat::NvsBootStore;

namespace {

constexpr char kNamespace[] = "logcat_test";

void erase_namespace() {
    nvs_handle_t handle;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(kNamespace, NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_all(handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
}

}  // namespace

void nvs_set_up() {
    erase_namespace();  // every test starts with no key: order does not matter
}

void test_begin() {
    NvsBootStore store(kNamespace);
    TEST_ASSERT_TRUE(store.begin());
}

void test_a_missing_key_is_missing_not_failed() {
    NvsBootStore store(kNamespace);
    TEST_ASSERT_TRUE(store.begin());
    uint32_t value = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(BootLoad::Missing), static_cast<int>(store.load(value)));
}

void test_save_and_load_back() {
    NvsBootStore store(kNamespace);
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_TRUE(store.save(41));
    uint32_t value = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(BootLoad::Found), static_cast<int>(store.load(value)));
    TEST_ASSERT_EQUAL_UINT32(41, value);
}

// What a reboot does: a new store (as at the next boot) reads what the last one committed.
void test_the_counter_goes_up_across_instances() {
    {
        NvsBootStore first(kNamespace);
        TEST_ASSERT_TRUE(first.begin());
        TEST_ASSERT_TRUE(first.save(41));
        TEST_ASSERT_EQUAL_UINT32(42, logcat::next_boot(first).boot);
    }
    NvsBootStore second(kNamespace);
    TEST_ASSERT_TRUE(second.begin());
    const logcat::BootInfo info = logcat::next_boot(second);
    TEST_ASSERT_EQUAL_UINT32(43, info.boot);
    TEST_ASSERT_TRUE(info.saved);
    TEST_ASSERT_FALSE(info.first);
}

void test_load_before_begin_fails() {
    NvsBootStore store(kNamespace);
    uint32_t value = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(BootLoad::Failed), static_cast<int>(store.load(value)));
    TEST_ASSERT_FALSE(store.save(1));
}

void run_nvs_tests() {
    Unity.TestFile = __FILE__;          // failures point at this file, not at test_main.cpp
    ESP_ERROR_CHECK(nvs_flash_init());  // the set-up needs NVS before the first test's begin()
    RUN_TEST(test_begin);
    RUN_TEST(test_a_missing_key_is_missing_not_failed);
    RUN_TEST(test_save_and_load_back);
    RUN_TEST(test_the_counter_goes_up_across_instances);
    RUN_TEST(test_load_before_begin_fails);
    erase_namespace();  // leave nothing behind in the test namespace
}
