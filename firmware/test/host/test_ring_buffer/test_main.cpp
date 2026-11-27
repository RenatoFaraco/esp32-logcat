#include <unity.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <type_traits>
#include <vector>

#include "logcat/log_record.hpp"
#include "logcat/ring_buffer.hpp"

using logcat::LogRecord;
using logcat::RingBuffer;
using IntBuffer = RingBuffer<uint32_t, 5>;

namespace {

uint32_t next_random(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

template <typename T, size_t N>
std::vector<T> contents(const RingBuffer<T, N>& buffer) {
    std::vector<T> items;
    buffer.for_each([&items](const T& item) { items.push_back(item); });
    return items;
}

void assert_contents(const IntBuffer& buffer, const std::vector<uint32_t>& expected) {
    const std::vector<uint32_t> actual = contents(buffer);
    TEST_ASSERT_EQUAL_size_t(expected.size(), actual.size());
    TEST_ASSERT_EQUAL_size_t(expected.size(), buffer.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT32(expected[i], actual[i]);
        TEST_ASSERT_NOT_NULL(buffer.at(i));
        TEST_ASSERT_EQUAL_UINT32(expected[i], *buffer.at(i));
    }
    TEST_ASSERT_NULL(buffer.at(expected.size()));
}

void push_range(IntBuffer& buffer, uint32_t first, uint32_t last) {
    for (uint32_t value = first; value <= last; ++value) {
        buffer.push(value);
    }
}

// The counter is the last 4 bytes of the object (offsetof checked at compile time in push()).
// Reading or writing it directly observes the state or simulates garbage in RTC memory.
template <typename Buffer>
uint32_t read_counter(const Buffer& buffer) {
    uint32_t counter = 0;
    std::memcpy(&counter, reinterpret_cast<const unsigned char*>(&buffer) + sizeof(Buffer) - 4, 4);
    return counter;
}

template <typename Buffer>
void force_counter(Buffer& buffer, uint32_t counter) {
    std::memcpy(reinterpret_cast<unsigned char*>(&buffer) + sizeof(Buffer) - 4, &counter, 4);
}

// Overwrites the raw bytes of slot `slot`, like a push interrupted in the middle of the copy.
void tear_slot(IntBuffer& buffer, size_t slot, uint32_t value) {
    std::memcpy(reinterpret_cast<unsigned char*>(&buffer) + slot * sizeof(uint32_t), &value, 4);
}

// Every access on a corrupted buffer must stay inside the item array.
void assert_contained(const IntBuffer& buffer) {
    const uint32_t* begin = reinterpret_cast<const uint32_t*>(&buffer);
    const uint32_t* end = begin + IntBuffer::capacity();
    TEST_ASSERT_TRUE(buffer.size() <= IntBuffer::capacity());
    size_t visits = 0;
    buffer.for_each([&visits, begin, end](const uint32_t& item) {
        TEST_ASSERT_TRUE(&item >= begin && &item < end);
        ++visits;
    });
    TEST_ASSERT_EQUAL_size_t(buffer.size(), visits);
    for (size_t i = 0; i < buffer.size(); ++i) {
        const uint32_t* item = buffer.at(i);
        TEST_ASSERT_TRUE(item >= begin && item < end);
    }
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- basic behavior

void test_new_buffer_is_empty_and_consistent() {
    IntBuffer buffer{};
    TEST_ASSERT_TRUE(buffer.empty());
    TEST_ASSERT_FALSE(buffer.full());
    TEST_ASSERT_TRUE(buffer.is_consistent());
    assert_contents(buffer, {});
}

// No constructor: a static instance is zero-initialized by the loader, which is empty.
void test_static_instance_starts_empty() {
    static IntBuffer buffer;
    TEST_ASSERT_TRUE(buffer.empty());
    TEST_ASSERT_TRUE(buffer.is_consistent());
}

void test_partial_fill_keeps_insertion_order() {
    IntBuffer buffer{};
    push_range(buffer, 1, 3);
    TEST_ASSERT_FALSE(buffer.full());
    assert_contents(buffer, {1, 2, 3});
}

void test_exactly_full() {
    IntBuffer buffer{};
    push_range(buffer, 1, 5);
    TEST_ASSERT_TRUE(buffer.full());
    TEST_ASSERT_TRUE(buffer.is_consistent());
    assert_contents(buffer, {1, 2, 3, 4, 5});
}

void test_wrap_overwrites_oldest() {
    IntBuffer buffer{};
    push_range(buffer, 1, 7);
    TEST_ASSERT_TRUE(buffer.full());
    assert_contents(buffer, {3, 4, 5, 6, 7});
}

void test_many_laps_keep_the_last_n() {
    IntBuffer buffer{};
    push_range(buffer, 1, 23);
    TEST_ASSERT_TRUE(buffer.is_consistent());
    assert_contents(buffer, {19, 20, 21, 22, 23});
}

void test_capacity_one_keeps_last_item() {
    RingBuffer<uint32_t, 1> buffer{};
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);
    TEST_ASSERT_TRUE(buffer.full());
    TEST_ASSERT_EQUAL_size_t(1, buffer.size());
    TEST_ASSERT_EQUAL_UINT32(3, *buffer.at(0));
    TEST_ASSERT_NULL(buffer.at(1));
}

void test_clear_empties_buffer() {
    IntBuffer buffer{};
    push_range(buffer, 1, 7);
    buffer.clear();
    TEST_ASSERT_TRUE(buffer.empty());
    TEST_ASSERT_TRUE(buffer.is_consistent());
    assert_contents(buffer, {});
    buffer.push(9);
    assert_contents(buffer, {9});
}

void test_capacity_is_constexpr() {
    static_assert(IntBuffer::capacity() == 5, "capacity usable at compile time");
    TEST_ASSERT_EQUAL_size_t(5, IntBuffer::capacity());
}

// ---------------------------------------------------------------- layout for RTC memory

void test_traits_and_exact_size() {
    using LogTail = RingBuffer<LogRecord, 64>;
    static_assert(std::is_trivially_default_constructible_v<LogTail>, "no code runs at boot");
    static_assert(std::is_trivially_copyable_v<LogTail>, "survives as raw bytes");
    // Aligned so the counter is written with one atomic 32-bit store.
    static_assert(alignof(LogTail) >= 4, "counter store must be a single aligned word");
    // Same size on the PC (64-bit) and on the ESP32-C3 (32-bit): uint32_t counter.
    TEST_ASSERT_EQUAL_size_t(64 * 160 + 4, sizeof(LogTail));
    TEST_ASSERT_EQUAL_size_t(5 * 4 + 4, sizeof(IntBuffer));
}

// The whole state is one counter in [0, 2N): it counts up to 2N - 1 and folds back to N.
void test_counter_folds_into_two_laps() {
    IntBuffer buffer{};
    const uint32_t expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 5, 6, 7, 8, 9, 5};
    for (uint32_t value : expected) {
        buffer.push(value);
        TEST_ASSERT_EQUAL_UINT32(value, read_counter(buffer));
    }
    buffer.clear();
    TEST_ASSERT_EQUAL_UINT32(0, read_counter(buffer));
}

// Counter in the full range with head at slot 2: oldest-to-newest walks the slots 2,3,4,0,1.
void test_full_state_walks_from_head() {
    IntBuffer buffer{};
    for (uint32_t slot = 0; slot < 5; ++slot) {
        tear_slot(buffer, slot, 100 + slot);
    }
    force_counter(buffer, 5 + 2);
    TEST_ASSERT_TRUE(buffer.is_consistent());
    assert_contents(buffer, {102, 103, 104, 100, 101});
}

// Reset in the middle of a push while filling: the item was being copied into the slot after
// the last one, but the counter was not published yet. Nothing visible changes.
void test_interrupted_push_while_filling_is_invisible() {
    for (uint32_t filled = 0; filled < 5; ++filled) {
        IntBuffer buffer{};
        push_range(buffer, 1, filled);
        tear_slot(buffer, filled, 0xDEADBEEF);
        TEST_ASSERT_TRUE(buffer.is_consistent());
        std::vector<uint32_t> expected;
        for (uint32_t value = 1; value <= filled; ++value)
            expected.push_back(value);
        assert_contents(buffer, expected);
    }
}

// Reset in the middle of a push when full: the slot being overwritten is the oldest item,
// still visible and half new. The buffer cannot tell; the RTC adapter's CRC must (#31).
void test_interrupted_push_when_full_shows_torn_oldest() {
    IntBuffer buffer{};
    push_range(buffer, 1, 7);
    tear_slot(buffer, 2, 0xDEADBEEF);  // slot of the next push = oldest item (3)
    TEST_ASSERT_TRUE(buffer.is_consistent());
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, *buffer.at(0));
}

void test_log_records_keep_order() {
    RingBuffer<LogRecord, 3> buffer{};
    const char* tags[] = {"A", "B", "C", "D", "E"};
    for (const char* tag : tags) {
        LogRecord record{};
        record.set_tag(tag);
        buffer.push(record);
    }
    TEST_ASSERT_EQUAL_STRING("C", buffer.at(0)->tag);
    TEST_ASSERT_EQUAL_STRING("D", buffer.at(1)->tag);
    TEST_ASSERT_EQUAL_STRING("E", buffer.at(2)->tag);
}

// Simulates RTC_NOINIT memory after a panic: the bytes stay, no constructor runs.
void test_bytes_copied_without_constructor_survive() {
    IntBuffer before{};
    push_range(before, 1, 7);

    IntBuffer after;  // default-init: indeterminate until the raw copy below
    std::memcpy(&after, &before, sizeof(IntBuffer));
    TEST_ASSERT_TRUE(after.is_consistent());
    assert_contents(after, {3, 4, 5, 6, 7});
}

// ---------------------------------------------------------------- corrupted state

void test_erased_or_random_state_is_detected_and_contained() {
    IntBuffer buffer;
    // void*: deliberately bypass the class invariants to simulate garbage RTC memory.
    std::memset(static_cast<void*>(&buffer), 0xFF, sizeof(buffer));
    TEST_ASSERT_FALSE(buffer.is_consistent());
    assert_contained(buffer);
}

void test_counter_outside_two_laps_is_inconsistent_and_contained() {
    const uint32_t bad[] = {10, 11, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF};
    for (uint32_t counter : bad) {
        IntBuffer buffer{};
        force_counter(buffer, counter);
        TEST_ASSERT_FALSE(buffer.is_consistent());
        assert_contained(buffer);
    }
    for (uint32_t counter = 0; counter < 10; ++counter) {
        IntBuffer buffer{};
        force_counter(buffer, counter);
        TEST_ASSERT_TRUE(buffer.is_consistent());
        assert_contained(buffer);
    }
}

// push on a corrupted buffer must not write outside it.
void test_push_on_corrupted_state_stays_inside() {
    struct Guarded {
        IntBuffer buffer;
        uint32_t canary[4];
    } guarded;
    std::memset(static_cast<void*>(&guarded.buffer), 0xFF, sizeof(guarded.buffer));
    for (uint32_t& word : guarded.canary)
        word = 0xA5A5A5A5;

    for (uint32_t value = 0; value < 12; ++value) {
        guarded.buffer.push(value);
        assert_contained(guarded.buffer);
        // A garbage counter is never "healed" into a valid state by push(): only clear() does.
        TEST_ASSERT_FALSE(guarded.buffer.is_consistent());
    }
    for (uint32_t word : guarded.canary)
        TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5, word);

    guarded.buffer.clear();
    TEST_ASSERT_TRUE(guarded.buffer.is_consistent());
    TEST_ASSERT_TRUE(guarded.buffer.empty());
}

// ---------------------------------------------------------------- property

// Thousands of random pushes and clears, checked against a trivial reference model.
void test_random_operations_match_reference_model() {
    RingBuffer<uint32_t, 7> buffer{};
    std::deque<uint32_t> model;
    uint32_t state = 99;
    for (int step = 0; step < 20000; ++step) {
        if (next_random(state) % 50 == 0) {
            buffer.clear();
            model.clear();
        } else {
            const uint32_t value = next_random(state);
            buffer.push(value);
            model.push_back(value);
            if (model.size() > 7) {
                model.pop_front();
            }
        }
        TEST_ASSERT_TRUE(buffer.is_consistent());
        TEST_ASSERT_EQUAL_size_t(model.size(), buffer.size());
        for (size_t i = 0; i < model.size(); ++i) {
            TEST_ASSERT_EQUAL_UINT32(model[i], *buffer.at(i));
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_new_buffer_is_empty_and_consistent);
    RUN_TEST(test_static_instance_starts_empty);
    RUN_TEST(test_partial_fill_keeps_insertion_order);
    RUN_TEST(test_exactly_full);
    RUN_TEST(test_wrap_overwrites_oldest);
    RUN_TEST(test_many_laps_keep_the_last_n);
    RUN_TEST(test_capacity_one_keeps_last_item);
    RUN_TEST(test_clear_empties_buffer);
    RUN_TEST(test_capacity_is_constexpr);
    RUN_TEST(test_traits_and_exact_size);
    RUN_TEST(test_counter_folds_into_two_laps);
    RUN_TEST(test_full_state_walks_from_head);
    RUN_TEST(test_interrupted_push_while_filling_is_invisible);
    RUN_TEST(test_interrupted_push_when_full_shows_torn_oldest);
    RUN_TEST(test_log_records_keep_order);
    RUN_TEST(test_bytes_copied_without_constructor_survive);
    RUN_TEST(test_erased_or_random_state_is_detected_and_contained);
    RUN_TEST(test_counter_outside_two_laps_is_inconsistent_and_contained);
    RUN_TEST(test_push_on_corrupted_state_stays_inside);
    RUN_TEST(test_random_operations_match_reference_model);
    return UNITY_END();
}
