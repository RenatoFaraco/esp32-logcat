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
    if (handle_ == nullptr || xQueueReceive(handle_, &record, timeout) != pdTRUE) {
        return false;
    }
    const uint32_t level = static_cast<uint32_t>(uxQueueMessagesWaiting(handle_)) + 1;
    if (level > high_water_.load(std::memory_order_relaxed)) {
        high_water_.store(level, std::memory_order_relaxed);
    }
    return true;
}

size_t FreeRtosLogQueue::used() const {
    return handle_ == nullptr ? 0 : uxQueueMessagesWaiting(handle_);
}

bool FreeRtosLogQueue::empty() const {
    return handle_ == nullptr || uxQueueMessagesWaiting(handle_) == 0;
}

}  // namespace logcat
