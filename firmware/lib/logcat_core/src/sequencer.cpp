#include "logcat/sequencer.hpp"

namespace logcat {

Sequencer::Sequencer(uint32_t first_seq) : next_seq_(first_seq % kSeqModulo), dropped_(0) {}

uint32_t Sequencer::next() {
    // compare_exchange instead of fetch_add: the wrap must happen exactly at kSeqModulo, not
    // when the 32-bit counter overflows (2^32 is not a multiple of 10^6).
    uint32_t current = next_seq_.load(std::memory_order_relaxed);
    uint32_t following = 0;
    do {
        following = (current + 1 == kSeqModulo) ? 0 : current + 1;
    } while (!next_seq_.compare_exchange_weak(current, following, std::memory_order_relaxed));
    return current;
}

void Sequencer::record_drop() {
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

uint32_t Sequencer::take_dropped() {
    // exchange reads and clears in one step: a drop recorded concurrently is never lost.
    return dropped_.exchange(0, std::memory_order_relaxed);
}

}  // namespace logcat
