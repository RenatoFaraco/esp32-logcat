#pragma once

#include "adapters/log_queue.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/writer.hpp"

namespace logcat {

// The low-priority task that drains the queue: format + sinks (Writer), and loss reports
// whenever the queue is empty. All slow I/O of logging happens here, never in the caller.
//
// Statically allocated (stack and TCB): no heap. Priority 1, the same as ordinary application
// tasks and below anything time-critical: logging must not change the timing of acquisition
// (PRD §5). Stack 4 KB: the Writer keeps its line buffer as a member; the sinks' stdio/driver
// calls are the deepest part (measure with uxTaskGetStackHighWaterMark).
//
// Logs produced *inside* this task (a driver reporting an error while we write to it) must
// not go back into the queue: IdfLogHook checks writer_task_handle() and prints them raw on
// the console instead.
TaskHandle_t start_writer_task(Writer& writer, FreeRtosLogQueue& queue);

TaskHandle_t writer_task_handle();

}  // namespace logcat
