#include <unity.h>

#include <cstdint>

#include "demo/latency_stats.hpp"

using demo::LatencyStats;
using demo::LatencySummary;

void setUp() {}
void tearDown() {}

static LatencyStats stats;  // 4 KB: static, as on the device

static LatencySummary fresh_with(const uint32_t* values, int count) {
    stats.reset();
    for (int i = 0; i < count; ++i) {
        stats.add(values[i]);
    }
    return stats.summary();
}

void test_empty_summary_is_all_zero() {
    stats.reset();
    const LatencySummary summary = stats.summary();
    TEST_ASSERT_EQUAL_UINT32(0, summary.count);
    TEST_ASSERT_EQUAL_UINT32(0, summary.min_us);
    TEST_ASSERT_EQUAL_UINT32(0, summary.p50_us);
    TEST_ASSERT_EQUAL_UINT32(0, summary.p99_us);
    TEST_ASSERT_EQUAL_UINT32(0, summary.max_us);
    TEST_ASSERT_EQUAL_UINT32(0, summary.overruns);
}

void test_single_value_is_every_statistic() {
    const uint32_t values[] = {7};
    const LatencySummary summary = fresh_with(values, 1);
    TEST_ASSERT_EQUAL_UINT32(1, summary.count);
    TEST_ASSERT_EQUAL_UINT32(7, summary.min_us);
    TEST_ASSERT_EQUAL_UINT32(7, summary.p50_us);
    TEST_ASSERT_EQUAL_UINT32(7, summary.p99_us);
    TEST_ASSERT_EQUAL_UINT32(7, summary.max_us);
}

// Nearest-rank percentile: the smallest value with at least q% of the samples at or below it.
void test_percentiles_use_the_nearest_rank() {
    stats.reset();
    for (uint32_t value = 1; value <= 100; ++value) {
        stats.add(value);
    }
    const LatencySummary summary = stats.summary();
    TEST_ASSERT_EQUAL_UINT32(100, summary.count);
    TEST_ASSERT_EQUAL_UINT32(1, summary.min_us);
    TEST_ASSERT_EQUAL_UINT32(50, summary.p50_us);
    TEST_ASSERT_EQUAL_UINT32(99, summary.p99_us);
    TEST_ASSERT_EQUAL_UINT32(100, summary.max_us);
}

void test_rare_outlier_shows_in_max_not_in_p99() {
    stats.reset();
    for (int i = 0; i < 999; ++i) {
        stats.add(4);
    }
    stats.add(250);
    const LatencySummary summary = stats.summary();
    TEST_ASSERT_EQUAL_UINT32(4, summary.p50_us);
    TEST_ASSERT_EQUAL_UINT32(4, summary.p99_us);
    TEST_ASSERT_EQUAL_UINT32(250, summary.max_us);
}

void test_p99_rounds_the_rank_up() {
    // 101 samples: rank ceil(101 * 0.99) = 100, so the 100th smallest value.
    stats.reset();
    for (uint32_t value = 1; value <= 101; ++value) {
        stats.add(value);
    }
    TEST_ASSERT_EQUAL_UINT32(100, stats.summary().p99_us);
    TEST_ASSERT_EQUAL_UINT32(51, stats.summary().p50_us);
}

void test_values_beyond_the_range_keep_the_exact_max() {
    stats.reset();
    stats.add(3);
    stats.add(LatencyStats::kRangeUs + 500);
    const LatencySummary summary = stats.summary();
    TEST_ASSERT_EQUAL_UINT32(2, summary.count);
    TEST_ASSERT_EQUAL_UINT32(1, summary.over_range);
    TEST_ASSERT_EQUAL_UINT32(LatencyStats::kRangeUs + 500, summary.max_us);
    // A percentile that falls in the over-range bin is reported as the max, never less.
    TEST_ASSERT_EQUAL_UINT32(LatencyStats::kRangeUs + 500, summary.p99_us);
}

void test_last_in_range_value() {
    stats.reset();
    stats.add(LatencyStats::kRangeUs - 1);
    const LatencySummary summary = stats.summary();
    TEST_ASSERT_EQUAL_UINT32(0, summary.over_range);
    TEST_ASSERT_EQUAL_UINT32(LatencyStats::kRangeUs - 1, summary.p50_us);
}

void test_overruns_accumulate_apart_from_samples() {
    stats.reset();
    stats.add(5);
    stats.add_overruns(2);
    stats.add_overruns(1);
    const LatencySummary summary = stats.summary();
    TEST_ASSERT_EQUAL_UINT32(1, summary.count);
    TEST_ASSERT_EQUAL_UINT32(3, summary.overruns);
}

void test_reset_forgets_everything() {
    stats.reset();
    stats.add(9);
    stats.add(LatencyStats::kRangeUs * 2);
    stats.add_overruns(4);
    stats.reset();
    stats.add(2);
    const LatencySummary summary = stats.summary();
    TEST_ASSERT_EQUAL_UINT32(1, summary.count);
    TEST_ASSERT_EQUAL_UINT32(2, summary.min_us);
    TEST_ASSERT_EQUAL_UINT32(2, summary.max_us);
    TEST_ASSERT_EQUAL_UINT32(2, summary.p99_us);
    TEST_ASSERT_EQUAL_UINT32(0, summary.over_range);
    TEST_ASSERT_EQUAL_UINT32(0, summary.overruns);
}

void test_min_ignores_insertion_order() {
    const uint32_t values[] = {30, 12, 0, 45};
    const LatencySummary summary = fresh_with(values, 4);
    TEST_ASSERT_EQUAL_UINT32(0, summary.min_us);
    TEST_ASSERT_EQUAL_UINT32(45, summary.max_us);
    TEST_ASSERT_EQUAL_UINT32(12, summary.p50_us);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_empty_summary_is_all_zero);
    RUN_TEST(test_single_value_is_every_statistic);
    RUN_TEST(test_percentiles_use_the_nearest_rank);
    RUN_TEST(test_rare_outlier_shows_in_max_not_in_p99);
    RUN_TEST(test_p99_rounds_the_rank_up);
    RUN_TEST(test_values_beyond_the_range_keep_the_exact_max);
    RUN_TEST(test_last_in_range_value);
    RUN_TEST(test_overruns_accumulate_apart_from_samples);
    RUN_TEST(test_reset_forgets_everything);
    RUN_TEST(test_min_ignores_insertion_order);
    return UNITY_END();
}
