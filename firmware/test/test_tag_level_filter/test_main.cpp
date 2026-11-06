#include <unity.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "logcat/log_record.hpp"
#include "logcat/tag_level_filter.hpp"

using logcat::LogLevel;
using logcat::TagLevelFilter;

// Compare levels by letter so a failure reads "Expected 'W' Was 'I'" ('?' for None).
#define ASSERT_LEVEL(expected, actual) \
    TEST_ASSERT_EQUAL_CHAR(logcat::level_to_char(expected), logcat::level_to_char(actual))

namespace {

const LogLevel kLineLevels[] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug,
                                LogLevel::Verbose};

// Global instance: the constexpr constructor makes it constant-initialized.
TagLevelFilter global_filter(LogLevel::Warn);

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- default level

void test_default_threshold_is_info() {
    const TagLevelFilter filter;
    ASSERT_LEVEL(LogLevel::Info, filter.threshold("ANY"));
    TEST_ASSERT_TRUE(filter.enabled("ANY", LogLevel::Error));
    TEST_ASSERT_TRUE(filter.enabled("ANY", LogLevel::Warn));
    TEST_ASSERT_TRUE(filter.enabled("ANY", LogLevel::Info));
    TEST_ASSERT_FALSE(filter.enabled("ANY", LogLevel::Debug));
    TEST_ASSERT_FALSE(filter.enabled("ANY", LogLevel::Verbose));
    TEST_ASSERT_EQUAL_size_t(0, filter.size());
}

void test_set_default_changes_tags_without_override() {
    TagLevelFilter filter;
    TEST_ASSERT_TRUE(filter.set("PRESSURE", LogLevel::Error));
    filter.set_default(LogLevel::Verbose);
    TEST_ASSERT_TRUE(filter.enabled("OTHER", LogLevel::Verbose));
    TEST_ASSERT_FALSE(filter.enabled("PRESSURE", LogLevel::Warn));
}

void test_star_sets_the_default() {
    TagLevelFilter filter;
    TEST_ASSERT_TRUE(filter.set("*", LogLevel::Debug));
    ASSERT_LEVEL(LogLevel::Debug, filter.threshold("ANY"));
    TEST_ASSERT_EQUAL_size_t(0, filter.size());
}

void test_global_constexpr_instance_is_usable() {
    ASSERT_LEVEL(LogLevel::Warn, global_filter.threshold("ANY"));
    TEST_ASSERT_TRUE(global_filter.set("BOOT", LogLevel::Info));
    TEST_ASSERT_TRUE(global_filter.enabled("BOOT", LogLevel::Info));
}

// ---------------------------------------------------------------- per-tag overrides

void test_override_applies_only_to_its_tag() {
    TagLevelFilter filter;
    TEST_ASSERT_TRUE(filter.set("PRESSURE", LogLevel::Debug));
    TEST_ASSERT_TRUE(filter.enabled("PRESSURE", LogLevel::Debug));
    TEST_ASSERT_FALSE(filter.enabled("PRESSURE", LogLevel::Verbose));
    TEST_ASSERT_FALSE(filter.enabled("OTHER", LogLevel::Debug));
    TEST_ASSERT_EQUAL_size_t(1, filter.size());
}

void test_updating_existing_tag_does_not_add_entry() {
    TagLevelFilter filter;
    TEST_ASSERT_TRUE(filter.set("PRESSURE", LogLevel::Debug));
    TEST_ASSERT_TRUE(filter.set("PRESSURE", LogLevel::Error));
    TEST_ASSERT_EQUAL_size_t(1, filter.size());
    ASSERT_LEVEL(LogLevel::Error, filter.threshold("PRESSURE"));
}

void test_enabled_is_threshold_inclusive_for_every_pair() {
    TagLevelFilter filter;
    for (LogLevel threshold : kLineLevels) {
        filter.set("T", threshold);
        for (LogLevel level : kLineLevels) {
            const bool expected = static_cast<uint8_t>(level) <= static_cast<uint8_t>(threshold);
            TEST_ASSERT_EQUAL(expected, filter.enabled("T", level));
        }
    }
}

// ---------------------------------------------------------------- table limits

void test_table_full_rejects_new_tags_but_updates_existing() {
    TagLevelFilter filter;
    char tag[8];
    for (size_t i = 0; i < logcat::kMaxTagFilters; ++i) {
        std::snprintf(tag, sizeof(tag), "T%02u", static_cast<unsigned>(i));
        TEST_ASSERT_TRUE(filter.set(tag, LogLevel::Debug));
    }
    TEST_ASSERT_EQUAL_size_t(TagLevelFilter::capacity(), filter.size());
    TEST_ASSERT_FALSE(filter.set("EXTRA", LogLevel::Debug));
    ASSERT_LEVEL(LogLevel::Info, filter.threshold("EXTRA"));
    TEST_ASSERT_TRUE(filter.set("T07", LogLevel::Error));
    ASSERT_LEVEL(LogLevel::Error, filter.threshold("T07"));
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxTagFilters, filter.size());
}

void test_null_and_empty_tags_are_rejected() {
    TagLevelFilter filter;
    TEST_ASSERT_FALSE(filter.set(nullptr, LogLevel::Debug));
    TEST_ASSERT_FALSE(filter.set("", LogLevel::Debug));
    TEST_ASSERT_EQUAL_size_t(0, filter.size());
    ASSERT_LEVEL(LogLevel::Info, filter.threshold(nullptr));
    TEST_ASSERT_FALSE(filter.enabled(nullptr, LogLevel::Debug));
}

// ---------------------------------------------------------------- tag matching

// ESP-IDF tags can exceed 15 chars; LogRecord keeps 15, so the filter matches on 15 too.
void test_long_tags_match_on_first_15_chars() {
    TagLevelFilter filter;
    TEST_ASSERT_TRUE(filter.set("esp-x509-crt-bundle", LogLevel::Error));
    ASSERT_LEVEL(LogLevel::Error, filter.threshold("esp-x509-crt-bundle"));
    ASSERT_LEVEL(LogLevel::Error, filter.threshold("esp-x509-crt-bu"));  // as stored in records
    ASSERT_LEVEL(LogLevel::Error, filter.threshold("esp-x509-crt-buXYZ"));
    ASSERT_LEVEL(LogLevel::Info, filter.threshold("esp-x509-crt-b"));
}

void test_prefixes_and_case_are_distinct() {
    TagLevelFilter filter;
    TEST_ASSERT_TRUE(filter.set("PRESSURE", LogLevel::Error));
    ASSERT_LEVEL(LogLevel::Info, filter.threshold("PRESS"));
    ASSERT_LEVEL(LogLevel::Info, filter.threshold("PRESSURE2"));
    ASSERT_LEVEL(LogLevel::Info, filter.threshold("pressure"));
    TEST_ASSERT_TRUE(filter.set("PRESS", LogLevel::Verbose));
    ASSERT_LEVEL(LogLevel::Error, filter.threshold("PRESSURE"));
    ASSERT_LEVEL(LogLevel::Verbose, filter.threshold("PRESS"));
}

// ---------------------------------------------------------------- None and invalid levels

void test_none_silences_a_tag() {
    TagLevelFilter filter;
    TEST_ASSERT_TRUE(filter.set("wifi", LogLevel::None));
    for (LogLevel level : kLineLevels) {
        TEST_ASSERT_FALSE(filter.enabled("wifi", level));
    }
    TEST_ASSERT_TRUE(filter.enabled("APP", LogLevel::Error));
}

void test_none_default_silences_everything_but_overrides() {
    TagLevelFilter filter(LogLevel::None);
    TEST_ASSERT_FALSE(filter.enabled("ANY", LogLevel::Error));
    TEST_ASSERT_TRUE(filter.set("APP", LogLevel::Warn));
    TEST_ASSERT_TRUE(filter.enabled("APP", LogLevel::Warn));
}

// None (0) and out-of-range values are thresholds or garbage, never the level of a line.
void test_invalid_line_levels_are_never_enabled() {
    TagLevelFilter filter(LogLevel::Verbose);
    TEST_ASSERT_FALSE(filter.enabled("ANY", LogLevel::None));
    TEST_ASSERT_FALSE(filter.enabled("ANY", static_cast<LogLevel>(99)));
    filter.set("T", LogLevel::None);
    TEST_ASSERT_FALSE(filter.enabled("T", LogLevel::None));
}

// ---------------------------------------------------------------- concurrency

// One writer (the console) adds every tag and keeps flipping their levels while readers (every
// task that logs) query them. A reader must only ever see the default or a level the writer
// stored for *that* tag: never a half-written tag matching the wrong entry.
void test_concurrent_readers_see_only_published_values() {
    TagLevelFilter filter;  // default Info
    std::atomic<bool> running{true};
    std::atomic<int> violations{0};
    std::atomic<long> reads{0};

    auto tag_name = [](size_t i, char (&out)[8]) {
        std::snprintf(out, sizeof(out), "TAG%02u", static_cast<unsigned>(i));
    };
    // Even tags only ever get Error/Debug, odd tags Warn/Verbose.
    auto allowed = [](size_t i, LogLevel level) {
        if (level == LogLevel::Info)
            return true;  // default before the tag is published
        return (i % 2 == 0) ? (level == LogLevel::Error || level == LogLevel::Debug)
                            : (level == LogLevel::Warn || level == LogLevel::Verbose);
    };

    std::thread writer([&] {
        char tag[8];
        for (int round = 0; round < 2000; ++round) {
            for (size_t i = 0; i < logcat::kMaxTagFilters; ++i) {
                tag_name(i, tag);
                const bool flip = (round % 2) != 0;
                const LogLevel level = (i % 2 == 0) ? (flip ? LogLevel::Debug : LogLevel::Error)
                                                    : (flip ? LogLevel::Verbose : LogLevel::Warn);
                filter.set(tag, level);
            }
        }
        running.store(false);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            char tag[8];
            while (running.load()) {
                for (size_t i = 0; i < logcat::kMaxTagFilters; ++i) {
                    tag_name(i, tag);
                    if (!allowed(i, filter.threshold(tag))) {
                        violations.fetch_add(1);
                    }
                    reads.fetch_add(1);
                }
            }
        });
    }
    writer.join();
    for (std::thread& reader : readers)
        reader.join();

    TEST_ASSERT_EQUAL_INT(0, violations.load());
    TEST_ASSERT_TRUE(reads.load() > 0);
    TEST_ASSERT_EQUAL_size_t(logcat::kMaxTagFilters, filter.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_default_threshold_is_info);
    RUN_TEST(test_set_default_changes_tags_without_override);
    RUN_TEST(test_star_sets_the_default);
    RUN_TEST(test_global_constexpr_instance_is_usable);
    RUN_TEST(test_override_applies_only_to_its_tag);
    RUN_TEST(test_updating_existing_tag_does_not_add_entry);
    RUN_TEST(test_enabled_is_threshold_inclusive_for_every_pair);
    RUN_TEST(test_table_full_rejects_new_tags_but_updates_existing);
    RUN_TEST(test_null_and_empty_tags_are_rejected);
    RUN_TEST(test_long_tags_match_on_first_15_chars);
    RUN_TEST(test_prefixes_and_case_are_distinct);
    RUN_TEST(test_none_silences_a_tag);
    RUN_TEST(test_none_default_silences_everything_but_overrides);
    RUN_TEST(test_invalid_line_levels_are_never_enabled);
    RUN_TEST(test_concurrent_readers_see_only_published_values);
    return UNITY_END();
}
