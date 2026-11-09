#pragma once

#include <cstdint>

#include "demo/latency_stats.hpp"
#include "freertos/FreeRTOS.h"

namespace app {

// The "critical" 1 kHz task of PRD §7 criterion 3 (#13): what logging must not delay.
//
// A hardware timer (gptimer, 1 MHz, alarm every 1000 µs with auto-reload) fires an ISR that
// toggles GPIO A and notifies the task; the task's first action is to toggle GPIO B. The
// FreeRTOS tick (100 Hz here) is too coarse for 1 ms, and a timer is what real acquisition code
// uses anyway.
//
// Measured per period, both from the same timer counter (it restarts at 0 on every alarm):
// - isr: µs from the alarm to the ISR (interrupts held off by critical sections, other ISRs);
// - wake: µs from the alarm to the task running (isr + scheduling + higher-priority work),
//   from the oldest alarm not served yet when periods were missed (overruns).
// A logic analyzer sees about the same as edges: A → B is wake − isr, A's own jitter is isr.
// The software numbers run a few µs above the edges: the driver captures the counter before
// the callback toggles A, and reading the counter from the task takes a few µs (soft capture).
struct CriticalReport {
    demo::LatencySummary isr;
    demo::LatencySummary wake;
    uint32_t stack_free_bytes;  // high-water mark since boot
};

inline constexpr uint32_t kCriticalPeriodUs = 1000;

// Configures GPIO A/B, starts the task and then the timer. false if any step failed (logged).
bool start_critical_task();

// The task logs one line per period itself (the "critical task logs" phase).
void set_critical_task_logging(bool enabled);

// Summary of the periods since the previous call, then the counts restart. Waits for the task
// to hand it over at the end of its next period (at most `timeout`); false if it did not.
// One caller at a time, and single-core only: after a timeout a late answer could clear the
// next request on a dual-core chip (v2 ports need a request id echoed with the mailbox).
bool take_critical_report(CriticalReport& report, TickType_t timeout);

}  // namespace app
