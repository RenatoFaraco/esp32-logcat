#include "demo/latency_stats.hpp"

#include <cstring>

namespace demo {

void LatencyStats::add(uint32_t latency_us) {
    if (latency_us < kRangeUs) {
        ++bins_[latency_us];
    } else {
        ++over_range_;
    }
    if (count_ == 0 || latency_us < min_) {
        min_ = latency_us;
    }
    if (latency_us > max_) {
        max_ = latency_us;
    }
    ++count_;
}

void LatencyStats::add_overruns(uint32_t missed) {
    overruns_ += missed;
}

uint32_t LatencyStats::percentile(uint32_t percent) const {
    // Nearest rank: the smallest value with at least `percent`% of the samples at or below it.
    const uint64_t rank = (static_cast<uint64_t>(count_) * percent + 99) / 100;
    uint64_t seen = 0;
    for (uint32_t value = 0; value < kRangeUs; ++value) {
        seen += bins_[value];
        if (seen >= rank) {
            return value;
        }
    }
    return max_;  // the rank falls in the over-range bin: the max is the only exact value there
}

LatencySummary LatencyStats::summary() const {
    if (count_ == 0) {
        return {0, 0, 0, 0, 0, 0, overruns_};
    }
    return {count_, min_, percentile(50), percentile(99), max_, over_range_, overruns_};
}

void LatencyStats::reset() {
    std::memset(bins_, 0, sizeof(bins_));
    over_range_ = 0;
    count_ = 0;
    min_ = 0;
    max_ = 0;
    overruns_ = 0;
}

}  // namespace demo
