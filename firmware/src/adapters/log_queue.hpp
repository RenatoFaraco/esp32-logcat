#pragma once

#include <atomic>
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

    // For `stats`. The high-water mark is sampled here, on the writer's side, so it costs the
    // callers nothing: lines only leave through receive(), so the level peaks right before one.
    size_t used() const;
    uint32_t high_water() const { return high_water_.load(std::memory_order_relaxed); }

private:
    StaticQueue_t control_{};
    uint8_t storage_[kDepth * sizeof(LogRecord)] = {};
    QueueHandle_t handle_ = nullptr;
    std::atomic<uint32_t> high_water_{0};
};

}  // namespace logcat
