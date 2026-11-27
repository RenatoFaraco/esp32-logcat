#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/reply_server.hpp"
#include "logcat/writer.hpp"
#include "port/log_queue.hpp"
#include "port/reply_queue.hpp"

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
//
// The console's replies come through here too: this task is the only owner of the USB sink, so
// a reply and a log line never mix bytes. Each turn it serves one reply entry (or one line of an
// open block), then at least one log entry, so neither queue starves (ADR-012 §5).
//
// `on_idle`, if given, runs in this task when its queue is empty, at least once a second under
// load, and between the steps of a reply or dump (the flash store's syncs, #24/#26): the sinks
// belong to this task, so their upkeep does too. `queue_empty` says whether every line logged
// so far has been written: a requested sync waits for that.
TaskHandle_t start_writer_task(Writer& writer, FreeRtosLogQueue& queue, ReplyServer& server,
                               FreeRtosReplyQueue& replies,
                               void (*on_idle)(bool queue_empty) = nullptr);

TaskHandle_t writer_task_handle();

}  // namespace logcat
