#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace logcat {

// Fixed-capacity circular buffer keeping the last N items; a push on a full buffer overwrites
// the oldest one. No heap.
//
// Designed for two uses:
// - `log tail` (M5): the last lines in RAM. NOT thread-safe: the writer task pushes and the
//   console reads, so the adapter must serialize access (mutex or critical section).
// - Last words (M6): the same buffer placed in RTC_NOINIT memory, which survives a panic or
//   watchdog reset. Hence:
//   * No constructor and no member initializers: the default constructor is trivial, so no
//     code runs at boot to wipe what survived. Static instances start zeroed (empty); declare
//     locals as `RingBuffer<T, N> buffer{}` to value-initialize them.
//   * After power-on, RTC memory holds garbage. Call is_consistent() before trusting the
//     content. Independently of that, every access is bounded by construction (`% N`,
//     `min(count, N)`), so even a garbage state never reads or writes outside the items.
//   * Magic number and CRC over the region belong to the RTC adapter (#31), not here.
template <typename T, size_t N>
class RingBuffer {
    static_assert(N > 0, "RingBuffer needs room for at least one item");
    static_assert(N < (size_t{1} << 31), "index arithmetic must not overflow on 32-bit targets");
    static_assert(std::is_trivially_copyable_v<T>, "items must survive as raw bytes");

public:
    // Appends an item, overwriting the oldest one when full.
    void push(const T& item) {
        const uint32_t slot = head_ % kCapacity;  // bounded even if head_ is garbage
        items_[slot] = item;
        head_ = (slot + 1) % kCapacity;
        if (count_ < kCapacity) {
            ++count_;
        }
    }

    // Number of stored items, never more than N (a garbage count is clamped).
    size_t size() const { return count_ < kCapacity ? count_ : kCapacity; }
    static constexpr size_t capacity() { return N; }
    bool empty() const { return size() == 0; }
    bool full() const { return size() == N; }

    // Forgets every item. Also the way to recover from an inconsistent state.
    void clear() {
        head_ = 0;
        count_ = 0;
    }

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

    // True when the indices describe a state that push()/clear() can produce: head inside the
    // array and, unless full, head == count (the buffer has not wrapped yet). Strict on
    // purpose, so random bytes almost always fail it.
    bool is_consistent() const {
        return head_ < kCapacity && (count_ == kCapacity || head_ == count_);
    }

private:
    static constexpr uint32_t kCapacity = static_cast<uint32_t>(N);

    size_t oldest_slot() const { return (head_ % N + N - size()) % N; }

    // Member order is part of the layout tests rely on: items first, then the two indices.
    // uint32_t (not size_t) keeps the same size on the PC and on the 32-bit target.
    T items_[N];
    uint32_t head_;   // slot of the next push
    uint32_t count_;  // items stored, up to N
};

}  // namespace logcat
