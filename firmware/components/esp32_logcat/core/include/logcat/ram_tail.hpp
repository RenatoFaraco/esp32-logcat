#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "logcat/line_formatter.hpp"
#include "logcat/ring_buffer.hpp"
#include "logcat/sink.hpp"

namespace logcat {

inline constexpr size_t kRamTailLines = 32;  // ~5.5 KB; what `log tail` returns (PRD F4)

// One formatted line, '\n' included, exactly as the other sinks got it.
struct TailLine {
    uint8_t length;
    char text[kMaxLineLength];
};
static_assert(kMaxLineLength <= UINT8_MAX, "TailLine::length is one byte");

// The last lines in RAM, as a sink of the writer.
//
// Owned by the writer task, which both pushes (write) and serves the block (ReplyServer), so
// there is no lock: nothing else may touch it, except reading skipped() from another task.
//
// While a block is being served the tail is frozen, so the byte count announced in #BEGIN stays
// true. Lines arriving then are skipped and counted, but accepted (true): a frozen tail is not
// a failing sink, and the Writer must not report it as one. Their seq shows the hole.
class RamTail final : public Sink {
public:
    bool write(const char* line, size_t length) override {
        if (frozen_) {
            skipped_.store(skipped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return true;
        }
        if (length > kMaxLineLength) {
            return false;  // the formatter never produces this
        }
        TailLine item{};
        item.length = static_cast<uint8_t>(length);
        std::memcpy(item.text, line, length);
        lines_.push(item);
        return true;
    }
    const char* name() const override { return "ram"; }

    void freeze() { frozen_ = true; }
    void unfreeze() { frozen_ = false; }

    size_t size() const { return lines_.size(); }
    const TailLine* at(size_t i) const { return lines_.at(i); }  // i = 0 is the oldest

    uint32_t total_bytes() const {
        uint32_t total = 0;
        lines_.for_each([&total](const TailLine& line) { total += line.length; });
        return total;
    }

    // Lines skipped while frozen, since boot. Safe from any task.
    uint32_t skipped() const { return skipped_.load(std::memory_order_relaxed); }

private:
    RingBuffer<TailLine, kRamTailLines> lines_{};
    bool frozen_ = false;
    std::atomic<uint32_t> skipped_{0};
};

}  // namespace logcat
