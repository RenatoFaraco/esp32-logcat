#include "app/critical_task.hpp"

#include <atomic>

#include "app/pins.hpp"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

namespace app {

namespace {

constexpr const char* TAG = "CRITICAL";
constexpr uint32_t kStackBytes = 4096;  // one ESP_LOGx call takes ~1.3 KB (#14 comment)
// Above every application task and the logcat writer (1), below the esp_timer task (22): a
// critical acquisition task in a real product sits about here.
constexpr UBaseType_t kPriority = 20;
constexpr uint32_t kTimerHz = 1'000'000;  // 1 tick = 1 µs
static_assert(sizeof(StackType_t) == 1, "ESP-IDF FreeRTOS: stack depth is in bytes");

StackType_t stack[kStackBytes];
StaticTask_t control;
TaskHandle_t task = nullptr;
gptimer_handle_t timer = nullptr;

// ISR -> task. The ISR is the only writer of these (plain load/store: no emulated RMW atomics in
// interrupt context on the C3); the task reads them, and takes the two "pending" ones with
// exchange (emulated with interrupts masked, so the ISR cannot run in the middle).
std::atomic<uint32_t> alarm_seq{0};           // alarms served by the ISR since boot
std::atomic<uint32_t> pending_isr_max_us{0};  // worst ISR latency since the task last looked
std::atomic<uint32_t> pending_isr_lost{0};    // periods the ISR itself was late by (>= 1 ms)
uint32_t timer_pin_level = 0;

std::atomic<bool> logging{false};
// Report handshake. Correct on this single-core chip because the caller (priority 2) cannot run
// while the task (20) fills the mailbox; a dual-core port needs a request id (critical_task.hpp).
std::atomic<bool> report_requested{false};
CriticalReport mailbox{};  // written by the task, read after report_requested goes false

// Owned by the task alone (LatencyStats is not thread-safe on purpose): 2 x 4 KB, static.
demo::LatencyStats isr_stats;
demo::LatencyStats wake_stats;

// Interrupt context: no logging, no blocking. In IRAM, but the interrupt is not allocated as
// cache-safe (CONFIG_GPTIMER_ISR_CACHE_SAFE is off): it is held off while the flash cache is
// disabled (flash writes, from M4 on), and that time shows up as ISR latency. gpio_ll is an
// inline write to the set/clear register (no read-modify-write shared with other pins).
bool IRAM_ATTR on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t* event, void*) {
    timer_pin_level ^= 1;
    gpio_ll_set_level(&GPIO, kPinTimer, timer_pin_level);
    // The counter restarted at 0 on the alarm, so its value now is the time since the alarm.
    // If the ISR came a whole period late or more, the alarms in between never fired: count them.
    const uint32_t latency = static_cast<uint32_t>(event->count_value);
    if (latency > pending_isr_max_us.load(std::memory_order_relaxed)) {
        pending_isr_max_us.store(latency, std::memory_order_relaxed);
    }
    if (latency >= kCriticalPeriodUs) {
        pending_isr_lost.store(
            pending_isr_lost.load(std::memory_order_relaxed) + latency / kCriticalPeriodUs,
            std::memory_order_relaxed);
    }
    alarm_seq.store(alarm_seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    return woken == pdTRUE;  // the driver yields to the task on return
}

void hand_over_report() {
    mailbox.isr = isr_stats.summary();
    mailbox.wake = wake_stats.summary();
    mailbox.stack_free_bytes = uxTaskGetStackHighWaterMark(nullptr);
    isr_stats.reset();
    wake_stats.reset();
    report_requested.store(false, std::memory_order_release);  // publishes the mailbox
}

void run(void*) {
    uint32_t pin_level = 0;
    uint32_t served_seq = alarm_seq.load(std::memory_order_acquire);
    uint32_t periods = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pin_level ^= 1;
        gpio_ll_set_level(&GPIO, kPinTask, pin_level);  // first thing: the analyzer's edge B

        // Read the counter between two reads of the alarm count: if an alarm fired in between,
        // the counter restarted and would turn a ~1000 µs latency into a tiny one. Retry then.
        uint32_t seq = 0;
        uint64_t since_alarm = 0;
        esp_err_t err = ESP_OK;
        do {
            seq = alarm_seq.load(std::memory_order_acquire);
            err = gptimer_get_raw_count(timer, &since_alarm);
        } while (seq != alarm_seq.load(std::memory_order_acquire));
        if (seq == served_seq) {
            continue;  // the notification of an alarm already counted by the previous period
        }
        // Alarms between the oldest one not served and the newest: whole periods missed. The
        // latency is measured from the oldest one.
        const uint32_t missed = seq - served_seq - 1;
        served_seq = seq;
        const uint32_t isr_max = pending_isr_max_us.exchange(0, std::memory_order_relaxed);
        const uint32_t isr_lost = pending_isr_lost.exchange(0, std::memory_order_relaxed);

        isr_stats.add(isr_max);
        if (err == ESP_OK) {
            uint32_t wake = static_cast<uint32_t>(since_alarm) + missed * kCriticalPeriodUs;
            // The task cannot run before the ISR: when the ISR was late past a period, the
            // counter restarted and only the ISR's own latency still shows how late it was.
            wake = wake > isr_max ? wake : isr_max;
            wake_stats.add(wake);
        }
        if (missed + isr_lost > 0) {
            wake_stats.add_overruns(missed + isr_lost);
        }
        ++periods;

        // The "work" of this demo task. Logging here is the worst case on purpose: the capture
        // (vsnprintf + parse + enqueue) runs in this task's own period.
        if (logging.load(std::memory_order_relaxed)) {
            ESP_LOGI(TAG, "period %lu", static_cast<unsigned long>(periods));
        }

        // Once per phase: ~100 µs of summaries and memset, still inside this 1 ms period.
        if (report_requested.load(std::memory_order_acquire)) {
            hand_over_report();
        }
    }
}

void release() {
    if (timer != nullptr) {
        gptimer_disable(timer);  // fails harmlessly if it was never enabled
        gptimer_del_timer(timer);
        timer = nullptr;
    }
    if (task != nullptr) {
        vTaskDelete(task);
        task = nullptr;
    }
}

}  // namespace

bool start_critical_task() {
    if (task != nullptr) {
        return true;
    }
    const gpio_config_t pins = {
        .pin_bit_mask = (1ULL << kPinTimer) | (1ULL << kPinTask),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&pins) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed");
        return false;
    }

    // The task first, then the timer: the ISR never sees a null task handle.
    task = xTaskCreateStatic(run, "critical", kStackBytes, nullptr, kPriority, stack, &control);

    const gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = kTimerHz,
        .intr_priority = 0,
        .flags = {},
    };
    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = kCriticalPeriodUs;
    alarm.reload_count = 0;
    alarm.flags.auto_reload_on_alarm = true;
    const gptimer_event_callbacks_t callbacks = {.on_alarm = on_alarm};

    esp_err_t err = gptimer_new_timer(&timer_config, &timer);
    if (err == ESP_OK) {
        err = gptimer_register_event_callbacks(timer, &callbacks, nullptr);
    }
    if (err == ESP_OK) {
        err = gptimer_set_alarm_action(timer, &alarm);
    }
    if (err == ESP_OK) {
        err = gptimer_enable(timer);
    }
    if (err == ESP_OK) {
        err = gptimer_start(timer);  // from here on the ISR runs every 1 ms
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer setup failed: %s", esp_err_to_name(err));
        release();  // a later call starts over instead of returning true with no timer
        return false;
    }
    return true;
}

void set_critical_task_logging(bool enabled) {
    logging.store(enabled, std::memory_order_relaxed);
}

bool take_critical_report(CriticalReport& report, TickType_t timeout) {
    report_requested.store(true, std::memory_order_release);
    const TickType_t start = xTaskGetTickCount();
    while (report_requested.load(std::memory_order_acquire)) {
        if (xTaskGetTickCount() - start >= timeout) {
            report_requested.store(false, std::memory_order_relaxed);
            return false;
        }
        vTaskDelay(1);
    }
    report = mailbox;
    return true;
}

}  // namespace app
