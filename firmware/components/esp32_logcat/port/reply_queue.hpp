#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "logcat/reply_server.hpp"

namespace logcat {

// The replies' own path to the writer task, next to the log queue and with another policy
// (ADR-012 §5): whoever replies is the console task, never a critical task, so it may wait for
// room. A reply going through the log queue could be dropped when that queue fills.
// Statically allocated: no heap.
class FreeRtosReplyQueue {
public:
    static constexpr size_t kDepth = 4;

    // Call once, before the console or the writer task use the queue.
    void begin();

    // Console task side: waits up to `timeout` for room. false = no room in time.
    bool send(const ReplyMessage& message, TickType_t timeout);

    // Writer task side: never waits, the task sleeps on the log queue instead.
    bool try_receive(ReplyMessage& message);

    bool empty() const;

private:
    StaticQueue_t control_{};
    uint8_t storage_[kDepth * sizeof(ReplyMessage)] = {};
    QueueHandle_t handle_ = nullptr;
};

}  // namespace logcat
