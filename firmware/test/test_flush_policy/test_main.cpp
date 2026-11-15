#include <unity.h>

#include <cstdint>
#include <initializer_list>

#include "logcat/flush_policy.hpp"

using logcat::FlushPolicy;
using logcat::LogLevel;
using Reason = logcat::FlushPolicy::Reason;

namespace {

constexpr uint32_t kPeriod = FlushPolicy::kPeriodMs;
constexpr uint32_t kGap = FlushPolicy::kMinGapMs;

void assert_reason(Reason expected, Reason actual) {
    TEST_ASSERT_EQUAL(static_cast<int>(expected), static_cast<int>(actual));
}

}  // namespace

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------------ period

void test_nothing_to_sync_before_the_period() {
    FlushPolicy policy(0);
    assert_reason(Reason::None, policy.on_idle(kPeriod - 1, 100));
}

void test_the_period_syncs_pending_bytes() {
    FlushPolicy policy(0);
    assert_reason(Reason::Period, policy.on_idle(kPeriod, 100));
}

void test_the_period_never_syncs_nothing() {
    FlushPolicy policy(0);
    assert_reason(Reason::None, policy.on_idle(10 * kPeriod, 0));
}

void test_the_period_counts_from_the_last_sync() {
    FlushPolicy policy(0);
    policy.synced(Reason::Period, kPeriod, true);
    assert_reason(Reason::None, policy.on_idle(2 * kPeriod - 1, 100));
    assert_reason(Reason::Period, policy.on_idle(2 * kPeriod, 100));
}

// The queue never empties under heavy logging, so on_idle never runs: the period must also be
// checked after each line, or the loss window grows without bound.
void test_the_period_is_also_checked_after_each_line() {
    FlushPolicy policy(0);
    assert_reason(Reason::None, policy.after_line(LogLevel::Info, kPeriod - 1, 100));
    assert_reason(Reason::Period, policy.after_line(LogLevel::Info, kPeriod, 100));
}

// ------------------------------------------------------------------ ERROR

void test_an_error_syncs_at_once() {
    FlushPolicy policy(0);
    assert_reason(Reason::Error, policy.after_line(LogLevel::Error, kGap, 100));
}

void test_warnings_and_below_do_not_sync() {
    FlushPolicy policy(0);
    for (LogLevel level : {LogLevel::Warn, LogLevel::Info, LogLevel::Debug, LogLevel::Verbose}) {
        assert_reason(Reason::None, policy.after_line(level, kGap, 100));
    }
}

// At most one sync a second, whatever the kind: an error storm costs one erase a second, and an
// error right after a periodic sync waits for the rest of that second.
void test_an_error_within_a_second_of_a_sync_waits() {
    FlushPolicy policy(0);
    policy.synced(Reason::Period, 10'000, true);
    assert_reason(Reason::None, policy.after_line(LogLevel::Error, 10'000 + kGap - 1, 100));
    assert_reason(Reason::Error, policy.on_idle(10'000 + kGap, 100));
}

void test_an_error_storm_syncs_once_a_second() {
    FlushPolicy policy(0);
    uint32_t syncs = 0;
    for (uint32_t now = kGap; now < kGap + 5000; now += 50) {  // 20 errors a second for 5 s
        const Reason reason = policy.after_line(LogLevel::Error, now, 100);
        if (reason != Reason::None) {
            policy.synced(reason, now, true);
            ++syncs;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(5, syncs);
    TEST_ASSERT_EQUAL_UINT32(5, policy.counts().error);
}

// A pending error is covered by any sync that comes after it: none is owed afterwards.
void test_any_sync_covers_a_pending_error() {
    FlushPolicy policy(0);
    policy.synced(Reason::Period, 10'000, true);
    policy.after_line(LogLevel::Error, 10'100, 100);  // pending
    policy.synced(Reason::Request, 10'200, true);     // `log sync`
    assert_reason(Reason::None, policy.on_idle(10'200 + kGap, 100));
}

void test_a_pending_error_with_nothing_unsynced_is_dropped() {
    FlushPolicy policy(0);
    policy.synced(Reason::Period, 10'000, true);
    policy.after_line(LogLevel::Error, 10'100, 100);
    assert_reason(Reason::None, policy.on_idle(10'000 + kGap, 0));  // nothing left to make durable
    assert_reason(Reason::None, policy.on_idle(10'000 + kGap + 1, 100));
}

// A failed sync did not make the error durable: it is tried again, a second later.
void test_a_failed_sync_keeps_the_error_pending() {
    FlushPolicy policy(0);
    TEST_ASSERT_TRUE(policy.after_line(LogLevel::Error, kGap, 100) == Reason::Error);
    policy.synced(Reason::Error, kGap, false);
    assert_reason(Reason::None, policy.on_idle(2 * kGap - 1, 100));
    assert_reason(Reason::Error, policy.on_idle(2 * kGap, 100));
}

// ------------------------------------------------------------------ clock and counts

void test_the_millisecond_clock_wraps() {
    const uint32_t start = UINT32_MAX - 100;
    FlushPolicy policy(start);
    assert_reason(Reason::None, policy.on_idle(start + kPeriod - 1, 100));  // wrapped past 0
    assert_reason(Reason::Period, policy.on_idle(start + kPeriod, 100));
}

void test_counts_by_reason() {
    FlushPolicy policy(0);
    policy.synced(Reason::Period, 1, true);
    policy.synced(Reason::Error, 2, true);
    policy.synced(Reason::Error, 3, false);
    policy.synced(Reason::Request, 4, true);
    TEST_ASSERT_EQUAL_UINT32(1, policy.counts().period);
    TEST_ASSERT_EQUAL_UINT32(2, policy.counts().error);
    TEST_ASSERT_EQUAL_UINT32(1, policy.counts().request);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_to_sync_before_the_period);
    RUN_TEST(test_the_period_syncs_pending_bytes);
    RUN_TEST(test_the_period_never_syncs_nothing);
    RUN_TEST(test_the_period_counts_from_the_last_sync);
    RUN_TEST(test_the_period_is_also_checked_after_each_line);
    RUN_TEST(test_an_error_syncs_at_once);
    RUN_TEST(test_warnings_and_below_do_not_sync);
    RUN_TEST(test_an_error_within_a_second_of_a_sync_waits);
    RUN_TEST(test_an_error_storm_syncs_once_a_second);
    RUN_TEST(test_any_sync_covers_a_pending_error);
    RUN_TEST(test_a_pending_error_with_nothing_unsynced_is_dropped);
    RUN_TEST(test_a_failed_sync_keeps_the_error_pending);
    RUN_TEST(test_the_millisecond_clock_wraps);
    RUN_TEST(test_counts_by_reason);
    return UNITY_END();
}
