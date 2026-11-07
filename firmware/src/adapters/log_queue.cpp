#include "adapters/log_queue.hpp"

namespace logcat {

void FreeRtosLogQueue::begin() {
    if (handle_ == nullptr) {
        handle_ = xQueueCreateStatic(kDepth, sizeof(LogRecord), storage_, &control_);
    }
}

bool FreeRtosLogQueue::try_send(const LogRecord& record) {
    return handle_ != nullptr && xQueueSend(handle_, &record, 0) == pdTRUE;
}

bool FreeRtosLogQueue::receive(LogRecord& record, TickType_t timeout) {
    return handle_ != nullptr && xQueueReceive(handle_, &record, timeout) == pdTRUE;
}

bool FreeRtosLogQueue::empty() const {
    return handle_ == nullptr || uxQueueMessagesWaiting(handle_) == 0;
}

}  // namespace logcat
