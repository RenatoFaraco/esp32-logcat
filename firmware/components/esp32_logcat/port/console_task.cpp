#include "port/console_task.hpp"

#include <cstdio>
#include <cstdlib>

#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_spi_flash_counters.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "logcat/boot_counter.hpp"
#include "port/idf_log_hook.hpp"
#include "port/reset_reason.hpp"
#include "port/writer_task.hpp"
#include "sdkconfig.h"

namespace logcat {

namespace {

constexpr const char* TAG = "CONSOLE";
constexpr uint32_t kStackBytes = 4096;  // ESP-IDF FreeRTOS: stack depth in bytes
constexpr UBaseType_t kPriority = 1;
constexpr TickType_t kReadWait = pdMS_TO_TICKS(100);
// The console may wait for room, but not past the host's own 2 s timeout (ADR-012 §6).
constexpr TickType_t kSendWait = pdMS_TO_TICKS(500);
// Long enough for the writer task to take what is queued and the driver to send it.
constexpr TickType_t kDrainWait = pdMS_TO_TICKS(100);
constexpr int kDrainPolls = 100;  // of 10 ms
constexpr uint32_t kProtocolVersion = 1;

StackType_t stack[kStackBytes];
StaticTask_t control;
Console* console_ptr = nullptr;

void run(void*) {
    char buffer[64];
    for (;;) {
        const int received = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), kReadWait);
        if (received > 0) {
            console_ptr->feed(buffer, static_cast<size_t>(received));
        }
    }
}

}  // namespace

bool EspConsolePlatform::send(const ReplyMessage& message) {
    const bool sent = replies_.send(message, kSendWait);
    queue_timeouts_ += sent ? 0 : 1;
    return sent;
}

void EspConsolePlatform::fill_info(ReplyFields& fields) {
    fields.add("fw", esp_app_get_description()->version);
    fields.add_uint("proto", kProtocolVersion);
    fields.add("chip", CONFIG_IDF_TARGET);
    fields.add("idf", esp_get_idf_version());
    fields.add_uint("uptime_ms", static_cast<uint32_t>(esp_timer_get_time() / 1000));
    if (writer_.boot() != kUnknownBoot) {  // unknown is left out, never sent as 0 (ADR-012 §2)
        fields.add_uint("boot", writer_.boot());
    }
    const char* reset = reset_name(esp_reset_reason());
    if (reset != nullptr) {
        fields.add("reset", reset);
    }
    fields.add("flash", to_string(flash_.snapshot().mount));
}

void EspConsolePlatform::fill_stats(ReplyFields& fields) {
    const CaptureStats capture = idf_log_hook_stats();
    char pair[24];
    fields.add_uint("dropped", capture.dropped_total);
    std::snprintf(pair, sizeof(pair), "%u/%u", static_cast<unsigned>(log_queue_.used()),
                  static_cast<unsigned>(FreeRtosLogQueue::kDepth));
    fields.add("queue", pair);
    fields.add_uint("queue_max", log_queue_.high_water());
    std::snprintf(pair, sizeof(pair), "%lu/%lu", static_cast<unsigned long>(capture.last_cycles),
                  static_cast<unsigned long>(capture.max_cycles));
    fields.add("capture_cycles", pair);
    fields.add_uint("writer_stack_free", uxTaskGetStackHighWaterMark(writer_task_handle()));
    fields.add_uint("console_stack_free", uxTaskGetStackHighWaterMark(nullptr));
    for (size_t i = 0; i < writer_.sink_count(); ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "not_sent.%s", writer_.sink_name(i));
        fields.add_uint(key, writer_.refused_total(i));
    }
    fields.add_uint("tail_skipped", tail_.skipped());
    fields.add_uint("reply_failed", server_.failures() + queue_timeouts_);
    // The heap's low-water mark: esp_littlefs allocates on every open (see the adapter).
    fields.add_uint("heap_min", static_cast<uint32_t>(esp_get_minimum_free_heap_size()));
    // The log in flash (#24): what the store counts, then what LittleFS itself uses.
    const FlashSnapshot flash = flash_.snapshot();
    if (flash.mount != MountResult::Failed) {
        fields.add_uint("flash.files", flash.store.files);
        fields.add_uint("flash.bytes", flash.store.bytes);
        fields.add_uint("flash.limit", flash.limit);
        fields.add_uint("flash.unsynced", flash.unsynced);
        fields.add_uint("flash.rotations", flash.store.rotations);
        fields.add_uint("flash.removed", flash.store.removed);
        fields.add_uint("flash.repaired", flash.store.repaired);
        fields.add_uint("flash.failed", flash.store.failed_writes);
        fields.add_uint("flash.sync_failed", flash.store.sync_failures);
        fields.add_uint("flash.lost", flash.store.lost_bytes);
        fields.add_uint("flash.sync_period", flash.syncs.period);
        fields.add_uint("flash.sync_error", flash.syncs.error);
        fields.add_uint("flash.sync_request", flash.syncs.request);
        size_t used = 0;
        size_t total = 0;
        if (flash_.usage(used, total)) {
            fields.add_uint("fs.used", static_cast<uint32_t>(used));
            fields.add_uint("fs.total", static_cast<uint32_t>(total));
        }
    }
    // Every 4 KB sector erased since boot, by anyone (the log, NVS): what wears the flash.
    fields.add_uint("erase.sectors", esp_flash_get_counters()->erase.bytes / 4096);
}

void EspConsolePlatform::set_log_level(const char* tag, LogLevel level) {
    // ESP-IDF drops a line by its own level before the hook sees it, so a D or V line needs
    // this gate open too. Numbers match (see log_record.hpp). Opening only the tag asked for
    // keeps the other ESP_LOGD of the IDF out of the hook. `log level * D` does open them all.
    // A tag cut to 15 chars does not match the full name here: it cannot go above the default.
    esp_log_level_set(tag, static_cast<esp_log_level_t>(level));
}

void EspConsolePlatform::reboot() {
    // Bounded: a stuck writer task is when a reboot is needed most. Under heavy logging the log
    // queue may never empty, and what is left in it at the end is lost.
    for (int i = 0; i < kDrainPolls && !(replies_.empty() && log_queue_.empty()); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(kDrainWait);
    // What was logged until now stays in flash. A sync with an erase, stretched by flash
    // auto suspend, can take a while: up to 2 s, then restart anyway.
    if (!flash_.request_sync(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "reboot: the last lines may not be in flash");
        vTaskDelay(kDrainWait);
    }
    usb_serial_jtag_wait_tx_done(kDrainWait);
    esp_restart();
}

SyncResult EspConsolePlatform::sync_log() {
    if (flash_.snapshot().mount == MountResult::Failed) {
        return SyncResult::Unsupported;
    }
    // Under the host's 2 s reply timeout, with room for queueing and sending the reply: the
    // writer serves the request once its queue is empty, at least once a second under load.
    return flash_.request_sync(pdMS_TO_TICKS(1200)) ? SyncResult::Ok : SyncResult::Failed;
}

ClearResult EspConsolePlatform::clear_log() {
    if (flash_.snapshot().mount == MountResult::Failed) {
        return ClearResult::Unsupported;
    }
    // Under the host's 2 s reply timeout, all steps together (at most 1.8 s).
    constexpr TickType_t kRepliesWait = pdMS_TO_TICKS(300);
    constexpr TickType_t kRequestWait = pdMS_TO_TICKS(900);
    constexpr TickType_t kTakenWait = pdMS_TO_TICKS(400);
    constexpr TickType_t kMarkerWait = pdMS_TO_TICKS(200);
    // Requests answer in order. A `log dump` queued just before this one must start first (and
    // make this one busy), not find the log already cleared: wait until the writer task has
    // taken every queued reply. It starts a dump in the same pass that takes its message.
    const TickType_t start = xTaskGetTickCount();
    while (!replies_.empty()) {
        if (xTaskGetTickCount() - start >= kRepliesWait) {
            return ClearResult::Busy;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    FlashStore::Cleared cleared{};
    const ClearResult result = flash_.request_clear(kRequestWait, kTakenWait, cleared);
    if (result == ClearResult::Ok) {
        // Usually the first line of the new log_000.txt (another task may log in between); its
        // boot and uptime say when. Synced before the answer, so a power cut right after the
        // `ok` does not leave a cleared log with nothing saying so.
        ESP_LOGW("LOGCAT", "log cleared by console: %lu files, %lu bytes removed",
                 static_cast<unsigned long>(cleared.files),
                 static_cast<unsigned long>(cleared.bytes));
        flash_.request_sync(kMarkerWait);
    }
    return result;
}

void EspConsolePlatform::crash_panic() {
    ESP_LOGE(TAG, "crash panic requested");
    vTaskDelay(kDrainWait);  // the line above reaches the host and the sinks
    abort();
}

void EspConsolePlatform::crash_wdt() {
    ESP_LOGE(TAG, "crash wdt requested");
    vTaskDelay(kDrainWait);
    portDISABLE_INTERRUPTS();  // no tick, no task switch: the interrupt watchdog fires
    for (;;) {
    }
}

TaskHandle_t start_console_task(Console& console) {
    console_ptr = &console;
    return xTaskCreateStatic(run, "logcat_console", kStackBytes, nullptr, kPriority, stack,
                             &control);
}

}  // namespace logcat
