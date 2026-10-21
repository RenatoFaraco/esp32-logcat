#include <unity.h>

#include "logcat/sequencer.hpp"

using logcat::Sequencer;

void setUp() {}
void tearDown() {}

void test_first_seq_is_zero_and_increments() {
    Sequencer sequencer;
    TEST_ASSERT_EQUAL_UINT32(0, sequencer.next());
    TEST_ASSERT_EQUAL_UINT32(1, sequencer.next());
    TEST_ASSERT_EQUAL_UINT32(2, sequencer.next());
}

// seq has 6 digits in the line format (PRD §3); at 1000 lines/s it wraps every ~17 min.
void test_seq_wraps_from_999999_to_zero() {
    Sequencer sequencer(logcat::kSeqModulo - 2);
    TEST_ASSERT_EQUAL_UINT32(999998, sequencer.next());
    TEST_ASSERT_EQUAL_UINT32(999999, sequencer.next());
    TEST_ASSERT_EQUAL_UINT32(0, sequencer.next());
    TEST_ASSERT_EQUAL_UINT32(1, sequencer.next());
}

void test_custom_first_seq_is_reduced_modulo() {
    Sequencer sequencer(logcat::kSeqModulo + 5);
    TEST_ASSERT_EQUAL_UINT32(5, sequencer.next());
}

void test_dropped_starts_at_zero() {
    Sequencer sequencer;
    TEST_ASSERT_EQUAL_UINT32(0, sequencer.take_dropped());
}

void test_record_drop_accumulates() {
    Sequencer sequencer;
    sequencer.record_drop();
    sequencer.record_drop();
    sequencer.record_drop();
    TEST_ASSERT_EQUAL_UINT32(3, sequencer.take_dropped());
}

void test_take_dropped_returns_count_and_resets() {
    Sequencer sequencer;
    sequencer.record_drop();
    TEST_ASSERT_EQUAL_UINT32(1, sequencer.take_dropped());
    TEST_ASSERT_EQUAL_UINT32(0, sequencer.take_dropped());
    sequencer.record_drop();
    TEST_ASSERT_EQUAL_UINT32(1, sequencer.take_dropped());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_seq_is_zero_and_increments);
    RUN_TEST(test_seq_wraps_from_999999_to_zero);
    RUN_TEST(test_custom_first_seq_is_reduced_modulo);
    RUN_TEST(test_dropped_starts_at_zero);
    RUN_TEST(test_record_drop_accumulates);
    RUN_TEST(test_take_dropped_returns_count_and_resets);
    return UNITY_END();
}
