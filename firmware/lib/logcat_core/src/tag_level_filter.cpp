#include "logcat/tag_level_filter.hpp"

namespace logcat {

namespace {

// Compares on the first 15 chars, like LogRecord stores tags. `stored` is NUL-terminated within
// kTagCapacity; `tag` may be longer than 15.
bool same_tag(const char* stored, const char* tag) {
    for (size_t i = 0; i < kTagCapacity - 1; ++i) {
        if (stored[i] != tag[i]) {
            return false;
        }
        if (stored[i] == '\0') {
            return true;
        }
    }
    return true;  // 15 equal chars: a longer tag matches its stored (cut) form
}

bool is_line_level(LogLevel level) {
    return level >= LogLevel::Error && level <= LogLevel::Verbose;
}

bool is_threshold(LogLevel level) {
    return level <= LogLevel::Verbose;  // None..Verbose; the enum is unsigned
}

// Forces constant initialization at compile time on every target that builds the core: if the
// constructor ever stops being constexpr-evaluable, this line fails to compile instead of a
// global instance silently falling back to dynamic initialization.
[[maybe_unused]] constexpr TagLevelFilter kConstantInitProbe{LogLevel::Warn};

}  // namespace

size_t TagLevelFilter::find(const char* tag) const {
    // Clamped: a count corrupted by a stray write elsewhere must not walk past the table.
    const uint32_t published = count_.load(std::memory_order_acquire);
    const size_t count = published < kMaxTagFilters ? published : kMaxTagFilters;
    for (size_t i = 0; i < count; ++i) {
        if (same_tag(entries_[i].tag, tag)) {
            return i;
        }
    }
    return kMaxTagFilters;
}

bool TagLevelFilter::set(const char* tag, LogLevel threshold) {
    if (tag == nullptr || tag[0] == '\0' || !is_threshold(threshold)) {
        return false;
    }
    if (tag[0] == '*' && tag[1] == '\0') {
        return set_default(threshold);
    }
    const uint8_t value = static_cast<uint8_t>(threshold);

    const size_t existing = find(tag);
    if (existing != kMaxTagFilters) {
        entries_[existing].level.store(value, std::memory_order_relaxed);
        return true;
    }

    // Single writer: nobody else can publish between this load and the store below.
    const uint32_t count = count_.load(std::memory_order_relaxed);
    if (count >= kMaxTagFilters) {
        return false;
    }
    Entry& entry = entries_[count];
    size_t i = 0;
    for (; i < kTagCapacity - 1 && tag[i] != '\0'; ++i) {
        entry.tag[i] = tag[i];
    }
    entry.tag[i] = '\0';
    entry.level.store(value, std::memory_order_relaxed);
    // Publish only after the tag and level are complete: release pairs with the acquire in
    // find(), so a reader that sees the new count also sees the whole entry.
    count_.store(count + 1, std::memory_order_release);
    return true;
}

bool TagLevelFilter::set_default(LogLevel threshold) {
    if (!is_threshold(threshold)) {
        return false;
    }
    default_.store(static_cast<uint8_t>(threshold), std::memory_order_relaxed);
    return true;
}

LogLevel TagLevelFilter::threshold(const char* tag) const {
    if (tag != nullptr) {
        const size_t index = find(tag);
        if (index != kMaxTagFilters) {
            return static_cast<LogLevel>(entries_[index].level.load(std::memory_order_relaxed));
        }
    }
    return static_cast<LogLevel>(default_.load(std::memory_order_relaxed));
}

bool TagLevelFilter::enabled(const char* tag, LogLevel level) const {
    return is_line_level(level) &&
           static_cast<uint8_t>(level) <= static_cast<uint8_t>(threshold(tag));
}

}  // namespace logcat
