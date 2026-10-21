#include <unity.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

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

void test_seq_starting_at_999999_wraps_on_the_next_call() {
    Sequencer sequencer(logcat::kSeqModulo - 1);
    TEST_ASSERT_EQUAL_UINT32(999999, sequencer.next());
    TEST_ASSERT_EQUAL_UINT32(0, sequencer.next());
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

// Many loggers call next() at once on the device. Every seq must be handed out exactly once.
void test_concurrent_next_never_repeats_a_seq() {
    constexpr int kThreads = 4;
    constexpr int kCallsPerThread = 20000;
    Sequencer sequencer;
    std::vector<std::vector<uint32_t>> taken(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&sequencer, &taken, t] {
            taken[t].reserve(kCallsPerThread);
            for (int i = 0; i < kCallsPerThread; ++i) {
                taken[t].push_back(sequencer.next());
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::vector<uint8_t> seen(kThreads * kCallsPerThread, 0);
    for (const std::vector<uint32_t>& values : taken) {
        for (uint32_t seq : values) {
            TEST_ASSERT_LESS_THAN_UINT32(seen.size(), seq);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, seen[seq], "seq handed out twice");
            seen[seq] = 1;
        }
    }
}

// The writer drains drops while loggers keep adding them: none may be lost or counted twice.
void test_concurrent_drops_are_never_lost() {
    constexpr int kThreads = 4;
    constexpr int kDropsPerThread = 20000;
    Sequencer sequencer;
    std::atomic<bool> producing{true};
    uint32_t drained = 0;

    std::thread writer([&sequencer, &producing, &drained] {
        while (producing.load()) {
            drained += sequencer.take_dropped();
        }
    });
    std::vector<std::thread> loggers;
    for (int t = 0; t < kThreads; ++t) {
        loggers.emplace_back([&sequencer] {
            for (int i = 0; i < kDropsPerThread; ++i) {
                sequencer.record_drop();
            }
        });
    }
    for (std::thread& logger : loggers) {
        logger.join();
    }
    producing.store(false);
    writer.join();
    drained += sequencer.take_dropped();

    TEST_ASSERT_EQUAL_UINT32(kThreads * kDropsPerThread, drained);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_seq_is_zero_and_increments);
    RUN_TEST(test_seq_wraps_from_999999_to_zero);
    RUN_TEST(test_seq_starting_at_999999_wraps_on_the_next_call);
    RUN_TEST(test_custom_first_seq_is_reduced_modulo);
    RUN_TEST(test_dropped_starts_at_zero);
    RUN_TEST(test_record_drop_accumulates);
    RUN_TEST(test_take_dropped_returns_count_and_resets);
    RUN_TEST(test_concurrent_next_never_repeats_a_seq);
    RUN_TEST(test_concurrent_drops_are_never_lost);
    return UNITY_END();
}
