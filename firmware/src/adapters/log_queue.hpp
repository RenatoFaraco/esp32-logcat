#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "logcat/log_record.hpp"

namespace logcat {

// Fixed-size FreeRTOS queue of LogRecords between the callers (IdfLogHook) and the writer task.
// Statically allocated: no heap, created once at boot (ARCHITECTURE §8: 32 x 160 B = 5 KB).
class FreeRtosLogQueue {
public:
    static constexpr size_t kDepth = 32;

    // Call once, before the hook or the writer task use the queue.
    void begin();

    // Callers' side: copies the record and returns at once. false = queue full, the caller
    // counts a drop (ADR-004). Never blocks, so it never slows down whoever logs.
    bool try_send(const LogRecord& record);

    // Writer task side: waits up to `timeout` for a record.
    bool receive(LogRecord& record, TickType_t timeout);

    bool empty() const;

private:
    StaticQueue_t control_{};
    uint8_t storage_[kDepth * sizeof(LogRecord)] = {};
    QueueHandle_t handle_ = nullptr;
};

}  // namespace logcat
