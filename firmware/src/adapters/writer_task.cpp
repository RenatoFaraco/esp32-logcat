#include "adapters/writer_task.hpp"

#include <atomic>

namespace logcat {

namespace {

constexpr uint32_t kStackBytes = 4096;  // ESP-IDF FreeRTOS: stack depth in bytes
constexpr UBaseType_t kPriority = 1;
// How long the task sleeps on an empty queue before checking for losses to report.
constexpr TickType_t kIdleWait = pdMS_TO_TICKS(100);
// Under sustained overload the queue never empties: still report losses at least this often,
// even if the report overtakes queued lines (the host treats that as reordering, ≤ 32).
constexpr TickType_t kMaxReportDelay = pdMS_TO_TICKS(1000);

StackType_t stack[kStackBytes];
StaticTask_t control;
std::atomic<TaskHandle_t> handle{nullptr};

struct Context {
    Writer* writer;
    FreeRtosLogQueue* queue;
};
Context context;

void run(void*) {
    // Also stored here: this task may run before xTaskCreateStatic returns to the creator.
    handle.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    LogRecord record{};
    TickType_t last_report = xTaskGetTickCount();
    for (;;) {
        if (context.queue->receive(record, kIdleWait)) {
            context.writer->write(record);
            const bool overdue = xTaskGetTickCount() - last_report >= kMaxReportDelay;
            if (!context.queue->empty() && !overdue) {
                continue;
            }
        }
        context.writer->idle();  // normally with the queue empty: reports do not overtake lines
        last_report = xTaskGetTickCount();
    }
}

}  // namespace

TaskHandle_t start_writer_task(Writer& writer, FreeRtosLogQueue& queue) {
    if (handle.load() != nullptr) {
        return handle.load();
    }
    context = Context{&writer, &queue};
    TaskHandle_t created =
        xTaskCreateStatic(run, "logcat_writer", kStackBytes, nullptr, kPriority, stack, &control);
    handle.store(created, std::memory_order_release);
    return created;
}

TaskHandle_t writer_task_handle() {
    return handle.load(std::memory_order_acquire);
}

}  // namespace logcat
