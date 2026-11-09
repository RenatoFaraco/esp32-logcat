#include "app/timing_sweep.hpp"

#include <cstdint>

#include "adapters/idf_log_hook.hpp"
#include "app/critical_task.hpp"
#include "app/pins.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app {

namespace {

constexpr const char* TAG = "TIMING";
constexpr uint32_t kPhaseSeconds = 10;
constexpr uint32_t kStackBytes = 3072;
constexpr UBaseType_t kPriority = 2;
constexpr uint32_t kTicksPerSecond = configTICK_RATE_HZ;
constexpr uint32_t kPulseUs = 5;

struct Phase {
    const char* name;
    uint32_t load_lines_per_s;  // from the load task
    bool critical_logs;         // the critical task logs one line per period (1000/s)
    bool uart_mirror;
};

constexpr Phase kPhases[] = {
    {"no log", 0, false, false},
    {"load 100/s", 100, false, false},
    {"load 500/s", 500, false, false},
    {"load 1000/s", 1000, false, false},
    {"critical logs 1000/s", 0, true, false},
    {"load 1000/s uart", 1000, false, true},
};
constexpr uint32_t kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

// The load comes in whole lines per tick: a rate that is not a multiple of the tick rate would
// be silently cut (100/s is 0 lines per tick at 1000 Hz).
constexpr bool rates_fit_the_tick() {
    for (const Phase& phase : kPhases) {
        if (phase.load_lines_per_s % kTicksPerSecond != 0) {
            return false;
        }
    }
    return true;
}
static_assert(rates_fit_the_tick(), "every load rate must be a multiple of configTICK_RATE_HZ");
static_assert(sizeof(StackType_t) == 1, "ESP-IDF FreeRTOS: stack depth is in bytes");

StackType_t stack[kStackBytes];
StaticTask_t control;
SwitchableSink* mirror = nullptr;

void mark_phase(uint32_t index) {
    for (uint32_t pulse = 0; pulse <= index; ++pulse) {
        gpio_set_level(kPinPhase, 1);
        esp_rom_delay_us(kPulseUs);
        gpio_set_level(kPinPhase, 0);
        esp_rom_delay_us(kPulseUs);
    }
}

void report(uint32_t index, const Phase& phase, const CriticalReport& r, uint32_t dropped) {
    // Two lines: one would pass the 124-char message limit (PRD §3).
    ESP_LOGI(TAG, "phase %lu %s: wake us p50 %lu p99 %lu max %lu, isr us p50 %lu p99 %lu max %lu",
             static_cast<unsigned long>(index), phase.name,
             static_cast<unsigned long>(r.wake.p50_us), static_cast<unsigned long>(r.wake.p99_us),
             static_cast<unsigned long>(r.wake.max_us), static_cast<unsigned long>(r.isr.p50_us),
             static_cast<unsigned long>(r.isr.p99_us), static_cast<unsigned long>(r.isr.max_us));
    // over 1 ms: samples past the histogram (then a p99 equal to max is the max, not a p99).
    ESP_LOGI(TAG,
             "phase %lu: periods %lu, overruns %lu, over 1 ms %lu, dropped %lu, stack free "
             "critical %lu B sweep %lu B",
             static_cast<unsigned long>(index), static_cast<unsigned long>(r.wake.count),
             static_cast<unsigned long>(r.wake.overruns),
             static_cast<unsigned long>(r.wake.over_range), static_cast<unsigned long>(dropped),
             static_cast<unsigned long>(r.stack_free_bytes),
             static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
}

void run_phase(uint32_t index, const Phase& phase) {
    mirror->set_enabled(phase.uart_mirror);
    set_critical_task_logging(phase.critical_logs);
    mark_phase(index);
    CriticalReport discarded{};
    if (!take_critical_report(discarded, pdMS_TO_TICKS(100))) {  // start counting from here
        ESP_LOGW(TAG, "phase %lu: no reset from the critical task, the counts include the gap",
                 static_cast<unsigned long>(index));
    }
    const uint32_t dropped_before = logcat::idf_log_hook_stats().dropped_total;

    const uint32_t per_tick = phase.load_lines_per_s / kTicksPerSecond;
    TickType_t wake = xTaskGetTickCount();
    for (uint32_t tick = 0; tick < kPhaseSeconds * kTicksPerSecond; ++tick) {
        for (uint32_t line = 0; line < per_tick; ++line) {
            ESP_LOGI("LOAD", "tick %lu line %lu", static_cast<unsigned long>(tick),
                     static_cast<unsigned long>(line));
        }
        vTaskDelayUntil(&wake, 1);
    }

    set_critical_task_logging(false);
    CriticalReport result{};
    const bool got = take_critical_report(result, pdMS_TO_TICKS(100));
    const uint32_t dropped = logcat::idf_log_hook_stats().dropped_total - dropped_before;
    mirror->set_enabled(false);  // the report is not part of the load: give the writer time
    vTaskDelay(pdMS_TO_TICKS(200));
    if (got) {
        report(index, phase, result, dropped);
    } else {
        ESP_LOGE(TAG, "phase %lu: the critical task did not answer",
                 static_cast<unsigned long>(index));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

void run(void*) {
    for (;;) {
        for (uint32_t index = 0; index < kPhaseCount; ++index) {
            run_phase(index, kPhases[index]);
        }
    }
}

}  // namespace

bool start_timing_sweep(SwitchableSink& uart_mirror) {
    mirror = &uart_mirror;
    const gpio_config_t pin = {
        .pin_bit_mask = 1ULL << kPinPhase,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&pin) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed");
        return false;
    }
    if (!start_critical_task()) {
        return false;
    }
    return xTaskCreateStatic(run, "timing_sweep", kStackBytes, nullptr, kPriority, stack,
                             &control) != nullptr;
}

}  // namespace app
