#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "logcat/log_record.hpp"

namespace logcat {

inline constexpr size_t kMaxTagFilters = 16;

// Per-tag log level threshold with a default for tags without an override (PRD F2), e.g.
// `log level PRESSURE D`, `log level wifi N` (None = silence), `log level * W` (default).
//
// Concurrency: lock-free readers, serialized writers.
// - enabled()/threshold() run on every log call, in any task, and never block (ADR-004).
// - Adding a NEW tag must be serialized: one writer task at a time (the console, or a mutex
//   among writers in the adapter). Two concurrent set() calls adding tags would race on the
//   same free entry, and could even rewrite an already published one.
// - Updating an EXISTING tag and set_default() are single atomic stores, safe from any task.
// A new tag is fully written before the entry count is published with a release store; readers
// load the count with acquire and only look at published entries, so they never see a
// half-written tag. Changing the level of an existing tag is an atomic 1-byte store.
// Entries are append-only: once published, a tag's bytes never change. That is what makes
// lock-free reads safe, so there is deliberately no remove()/clear(); to drop an override, set
// it to the default level. Only atomic loads/stores are used (no read-modify-write), which on
// the ESP32-C3 are plain instructions, not the IDF's emulated __atomic_* calls.
//
// Tags are compared on their first 15 chars, the length LogRecord keeps, so long ESP-IDF tags
// (e.g. "esp-x509-crt-bundle") match their stored form. Case-sensitive.
class TagLevelFilter {
public:
    // constexpr so a global instance is constant-initialized (no static-init order issue).
    constexpr explicit TagLevelFilter(LogLevel default_threshold = LogLevel::Info)
        : entries_{}, count_(0), default_(static_cast<uint8_t>(default_threshold)) {}

    // Sets the threshold of `tag`; "*" sets the default. Returns false for a null or empty tag,
    // a threshold outside None..Verbose, or a full table with a new `tag` (existing tags can
    // always be updated). [[nodiscard]]: a full table must be reported, not ignored.
    [[nodiscard]] bool set(const char* tag, LogLevel threshold);

    // Returns false (and keeps the old default) for a threshold outside None..Verbose.
    [[nodiscard]] bool set_default(LogLevel threshold);

    // Threshold that applies to `tag`: its override, or the default.
    LogLevel threshold(const char* tag) const;

    // True if a line of `level` from `tag` should be logged. Only E/W/I/D/V lines can pass;
    // None and out-of-range values are never enabled.
    bool enabled(const char* tag, LogLevel level) const;

    // Number of tag overrides.
    size_t size() const { return count_.load(std::memory_order_acquire); }
    static constexpr size_t capacity() { return kMaxTagFilters; }

private:
    struct Entry {
        char tag[kTagCapacity];  // first 15 chars + '\0'
        std::atomic<uint8_t> level;
    };

    // Index of the published entry matching `tag`, or kMaxTagFilters.
    size_t find(const char* tag) const;

    Entry entries_[kMaxTagFilters];
    std::atomic<uint32_t> count_;
    std::atomic<uint8_t> default_;
};

}  // namespace logcat
