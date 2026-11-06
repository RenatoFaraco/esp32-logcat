#pragma once

#include <cstdint>

#include "logcat/sequencer.hpp"
#include "logcat/tag_level_filter.hpp"

namespace logcat {

// Cost of the capture step of the hook, in CPU cycles: vsnprintf + parse + building the record.
// This is the part that stays in the caller's context once the queue (#11) exists; the output
// is excluded. Approximate on purpose: plain relaxed loads/stores, no read-modify-write.
struct CaptureStats {
    uint32_t last_cycles;
    uint32_t max_cycles;
};

// Routes every ESP_LOGx (application and ESP-IDF) through the logcat core: the formatted line is
// recognized (parse_idf_text), filtered per tag, numbered and written in the PRD §3 format.
// Anything that is not a log line is passed to the previous output unchanged.
//
// PROVISIONAL (#10): the line is written synchronously, in the caller's context, through the
// previous vprintf. The non-blocking queue (#11) and the writer task (#12) replace this.
//
// Install it as early as possible in app_main; logs printed before keep the ESP-IDF format.
// `sequencer` and `filter` must outlive the hook (use globals: both have constexpr constructors).
void install_idf_log_hook(Sequencer& sequencer, TagLevelFilter& filter);

CaptureStats idf_log_hook_stats();

}  // namespace logcat
