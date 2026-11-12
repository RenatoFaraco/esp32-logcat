#include "adapters/reply_queue.hpp"

namespace logcat {

void FreeRtosReplyQueue::begin() {
    if (handle_ == nullptr) {
        handle_ = xQueueCreateStatic(kDepth, sizeof(ReplyMessage), storage_, &control_);
    }
}

bool FreeRtosReplyQueue::send(const ReplyMessage& message, TickType_t timeout) {
    return handle_ != nullptr && xQueueSend(handle_, &message, timeout) == pdTRUE;
}

bool FreeRtosReplyQueue::try_receive(ReplyMessage& message) {
    return handle_ != nullptr && xQueueReceive(handle_, &message, 0) == pdTRUE;
}

bool FreeRtosReplyQueue::empty() const {
    return handle_ == nullptr || uxQueueMessagesWaiting(handle_) == 0;
}

}  // namespace logcat
