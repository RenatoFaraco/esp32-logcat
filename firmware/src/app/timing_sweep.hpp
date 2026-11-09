#pragma once

#include "app/switchable_sink.hpp"

namespace app {

// The measurement loop of #13/#14, run forever by its own task: each phase sets a log load,
// runs for kPhaseSeconds with the critical task measuring, then logs two TIMING lines with
// that phase's latencies, overruns, dropped lines and the critical task's stack.
//
// Phases: no log; 100, 500 and 1000 lines/s from a load task; 1000 lines/s logged by the
// critical task itself; 1000 lines/s with the UART mirror on. The mirror is off in the
// others (it drains ~70 lines/s). At the start of phase N, GPIO C gives N + 1 short pulses so a
// logic analyzer capture can be cut into phases.
//
// The load task runs at priority 2: below the critical task, above the logcat writer (1), so
// the writer drains the queue only while the load task sleeps between batches. The FreeRTOS
// tick is 10 ms, so the load comes in batches of rate/100 lines per tick (10 at 1000 lines/s,
// under the 32-deep queue).
//
// Not silent in "no log": the heartbeat (1 line/s) and the TIMING lines between phases still
// go through the queue. That is the baseline a product with some logging would have.
bool start_timing_sweep(SwitchableSink& uart_mirror);

}  // namespace app
