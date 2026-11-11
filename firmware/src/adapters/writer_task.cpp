#include "adapters/writer_task.hpp"

#include <atomic>

namespace logcat {

namespace {

constexpr uint32_t kStackBytes = 4096;  // ESP-IDF FreeRTOS: stack depth in bytes
constexpr UBaseType_t kPriority = 1;
// How long the task sleeps on an empty queue before checking for replies and losses to report.
// It sleeps on the log queue alone: a queue set would also wake it for replies, but would add
// cost to every xQueueSend of whoever logs, the path the timing criterion measures (#13).
constexpr TickType_t kIdleWait = pdMS_TO_TICKS(20);
// Under sustained overload the queue never empties: still report losses at least this often,
// even if the report overtakes queued lines (the host treats that as reordering, ≤ 32).
constexpr TickType_t kMaxReportDelay = pdMS_TO_TICKS(1000);

StackType_t stack[kStackBytes];
StaticTask_t control;
std::atomic<TaskHandle_t> handle{nullptr};

struct Context {
    Writer* writer;
    FreeRtosLogQueue* queue;
    ReplyServer* server;
    FreeRtosReplyQueue* replies;
};
Context context;

void run(void*) {
    // Also stored here: this task may run before xTaskCreateStatic returns to the creator.
    handle.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    static ReplyMessage reply;  // ~180 B: off this task's stack
    LogRecord record{};
    TickType_t last_report = xTaskGetTickCount();
    for (;;) {
        if (context.server->block_open()) {
            context.server->step();
        } else if (context.replies->try_receive(reply)) {
            context.server->handle(reply);
        }
        const bool replying = context.server->block_open() || !context.replies->empty();
        if (context.queue->receive(record, replying ? 0 : kIdleWait)) {
            context.writer->write(record);
            const bool overdue = xTaskGetTickCount() - last_report >= kMaxReportDelay;
            if (!context.queue->empty() && !overdue) {
                continue;
            }
        } else if (replying) {
            continue;  // nothing to log, more to reply: no reason to look for losses yet
        }
        context.writer->idle();  // normally with the queue empty: reports do not overtake lines
        last_report = xTaskGetTickCount();
    }
}

}  // namespace

TaskHandle_t start_writer_task(Writer& writer, FreeRtosLogQueue& queue, ReplyServer& server,
                               FreeRtosReplyQueue& replies) {
    if (handle.load() != nullptr) {
        return handle.load();
    }
    context = Context{&writer, &queue, &server, &replies};
    TaskHandle_t created =
        xTaskCreateStatic(run, "logcat_writer", kStackBytes, nullptr, kPriority, stack, &control);
    handle.store(created, std::memory_order_release);
    return created;
}

TaskHandle_t writer_task_handle() {
    return handle.load(std::memory_order_acquire);
}

}  // namespace logcat
