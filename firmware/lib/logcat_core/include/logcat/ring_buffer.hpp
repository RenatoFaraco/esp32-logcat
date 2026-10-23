#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace logcat {

// Fixed-capacity circular buffer keeping the last N items; a push on a full buffer overwrites
// the oldest one. No heap.
//
// Designed for two uses:
// - `log tail` (M5): the last lines in RAM. NOT thread-safe: the writer task pushes and the
//   console reads, so the adapter must serialize access. Pointers from at() and references
//   given to for_each() are valid only while that lock is held: copy an item out under the
//   lock and do slow I/O after releasing it.
// - Last words (M6): the same buffer placed in RTC_NOINIT memory, which survives a panic or
//   watchdog reset. Hence:
//   * No constructor and no member initializers: the default constructor is trivial, so no
//     code runs at boot to wipe what survived. Instances in .bss start zeroed (empty); locals
//     must be value-initialized (`RingBuffer<T, N> buffer{}`). An RTC_NOINIT instance must be
//     declared WITHOUT `{}`, which could add a zeroing initializer that runs at every boot.
//   * The whole state is one counter, so push() publishes a new state with a single aligned
//     32-bit store: a reset in the middle can never leave a mismatched pair of indices. If it
//     hits while the buffer is filling, the half-written slot is outside size() and invisible;
//     when full it is the oldest item, which only the adapter's CRC can detect.
//   * After power-on, RTC memory holds garbage. is_consistent() rejects most of it, but the
//     integrity guarantee comes from the adapter's magic number + CRC over the stored region
//     (#31). Independently, every access is bounded by construction (`% N`, `min(count, N)`),
//     so even a garbage state never reads or writes outside the items.
template <typename T, size_t N>
class RingBuffer {
    static_assert(N > 0, "RingBuffer needs room for at least one item");
    static_assert(N < (size_t{1} << 31), "2 * N must fit in the 32-bit counter");
    static_assert(std::is_trivially_copyable_v<T>, "items must survive as raw bytes");
    static_assert(std::is_trivially_default_constructible_v<T>,
                  "no code may run at boot: a member initializer in T would wipe RTC_NOINIT");
    static_assert(std::is_standard_layout_v<T>, "fixed layout for RTC memory and offsetof");

public:
    // Appends an item, overwriting the oldest one when full.
    void push(const T& item) {
        static_assert(offsetof(RingBuffer, written_) == sizeof(T) * N,
                      "layout: items first, then the counter (tests and RTC rely on it)");
        items_[written_ % kCapacity] = item;  // bounded even if written_ is garbage
        // The item must be complete before the new state is published, even if the compiler
        // would like to reorder: a panic may stop the CPU between these two lines.
        std::atomic_signal_fence(std::memory_order_release);
        if (written_ < kTwiceCapacity) {
            // Counts up to 2N - 1, then folds back to N: head = written % N keeps advancing and
            // count stays N. A garbage counter is left as is (inconsistent until clear()).
            written_ = (written_ + 1 == kTwiceCapacity) ? kCapacity : written_ + 1;
        }
    }

    // Number of stored items, never more than N (a garbage counter is clamped).
    size_t size() const { return written_ < kCapacity ? written_ : kCapacity; }
    static constexpr size_t capacity() { return N; }
    bool empty() const { return size() == 0; }
    bool full() const { return size() == N; }

    // Forgets every item. Also the way to recover from an inconsistent state.
    void clear() { written_ = 0; }

    // Item at position i, counting from the oldest (i = 0); nullptr when i >= size().
    const T* at(size_t i) const {
        if (i >= size()) {
            return nullptr;
        }
        return &items_[(oldest_slot() + i) % N];
    }

    // Visits the items from the oldest to the newest. Template instead of std::function, which
    // may allocate.
    template <typename Visitor>
    void for_each(Visitor&& visit) const {
        const size_t first = oldest_slot();
        const size_t n = size();
        for (size_t i = 0; i < n; ++i) {
            visit(items_[(first + i) % N]);
        }
    }

    // True when the counter is one push()/clear() can produce. Not an integrity check: a
    // random counter passes with probability 2N / 2^32. RTC data also needs magic + CRC.
    bool is_consistent() const { return written_ < kTwiceCapacity; }

private:
    static constexpr uint32_t kCapacity = static_cast<uint32_t>(N);
    static constexpr uint32_t kTwiceCapacity = 2 * kCapacity;

    size_t oldest_slot() const { return (written_ % N + N - size()) % N; }

    // Member order is part of the layout (checked in push()): items first, then the counter.
    // uint32_t (not size_t) keeps the same size on the PC and on the 32-bit target.
    T items_[N];
    uint32_t written_;  // pushes since clear(), folded into [0, 2N); head = % N, count = min(N)
};

}  // namespace logcat
