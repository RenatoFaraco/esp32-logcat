#pragma once

#include <cstdint>

namespace demo {

struct LatencySummary {
    uint32_t count;  // samples added
    uint32_t min_us;
    uint32_t p50_us;  // nearest-rank percentiles
    uint32_t p99_us;
    uint32_t max_us;      // exact, even beyond the histogram range
    uint32_t over_range;  // samples >= kRangeUs
    uint32_t overruns;    // periods missed entirely (counted apart, not as samples)
};

// Latency histogram for the demo's 1 kHz task (#13, #14): 1 µs bins up to kRangeUs, one
// over-range bin, exact min and max. No heap: 4 KB of bins, meant to live in static memory.
//
// Not thread-safe on purpose: a single task owns it (the critical task adds a sample per period
// and, when asked, takes the summary and resets). Nothing else touches it, so no lock or atomic
// sits on the measured path.
class LatencyStats {
public:
    static constexpr uint32_t kRangeUs = 1000;  // one period of the 1 kHz task

    void add(uint32_t latency_us);
    void add_overruns(uint32_t missed);
    // O(kRangeUs): call it once per measurement phase, not per sample.
    LatencySummary summary() const;
    void reset();

private:
    uint32_t percentile(uint32_t percent) const;

    uint32_t bins_[kRangeUs] = {};
    uint32_t over_range_ = 0;
    uint32_t count_ = 0;
    uint32_t min_ = 0;
    uint32_t max_ = 0;
    uint32_t overruns_ = 0;
};

}  // namespace demo
