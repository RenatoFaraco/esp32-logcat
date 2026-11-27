// The C API (include/esp32_logcat.h) over the C++ core and adapters: the whole pipeline,
// statically allocated, and the order it must start in.
//
//   callers --ESP_LOGx--> hook --queue--> writer task --> sinks: USB, UART mirror, RAM tail, flash
//   console task <-- USB requests;  replies --> reply queue --> writer task --> USB

#include "esp32_logcat.h"

#include <atomic>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logcat/boot_counter.hpp"
#include "logcat/console.hpp"
#include "logcat/ram_tail.hpp"
#include "logcat/reply_server.hpp"
#include "logcat/sequencer.hpp"
#include "logcat/tag_level_filter.hpp"
#include "logcat/writer.hpp"
#include "port/console_sink.hpp"
#include "port/console_task.hpp"
#include "port/esp_clock.hpp"
#include "port/filter_lock.hpp"
#include "port/flash_store.hpp"
#include "port/idf_log_hook.hpp"
#include "port/log_queue.hpp"
#include "port/nvs_boot_store.hpp"
#include "port/reply_queue.hpp"
#include "port/reset_reason.hpp"
#include "port/switchable_sink.hpp"
#include "port/usb_jtag_sink.hpp"
#include "port/writer_task.hpp"

namespace logcat {
namespace {

constexpr const char* TAG = "LOGCAT";

// Constant-initialized (constexpr constructors): ready before app_main and the hook.
Sequencer sequencer;
TagLevelFilter tag_filter(LogLevel::Info);
// The logging pipeline: callers -> queue -> writer task -> sinks. All statically allocated.
FreeRtosLogQueue log_queue;
EspClock uptime_clock;
Writer writer(sequencer, uptime_clock);
UsbJtagSink usb_sink;      // native USB port: the logcat stream
ConsoleSink console_sink;  // UART console: mirror for monitors (ROM and bootloader print there)
SwitchableSink uart_mirror(console_sink);
// The console: requests in on the native port, replies out through the writer task.
GatedSink usb_log(usb_sink);  // out of the log while a dump block is open
RamTail ram_tail;             // what `log tail` returns
FreeRtosReplyQueue reply_queue;
ReplyServer reply_server(usb_sink, usb_log, ram_tail);
// The log in flash (#24): LittleFS on the `logs` partition, synced by the FlushPolicy (#26).
FlashStore flash_store;
EspConsolePlatform console_platform(reply_queue, log_queue, writer, reply_server, ram_tail,
                                    flash_store);
Console console(console_platform, tag_filter);
NvsBootStore boot_store;

StaticSemaphore_t filter_mutex_storage;
SemaphoreHandle_t filter_mutex = nullptr;
std::atomic<bool> started{false};
uint32_t this_boot = kUnknownBoot;  // written once by init, before `started`

// Who this boot is (#25): the first line the board keeps.
void log_boot(const BootInfo& boot, bool nvs_erased) {
    if (boot.boot == kUnknownBoot) {
        ESP_LOGE(TAG, "boot counter unavailable: lines carry boot 0000");
    }
    if (nvs_erased) {
        ESP_LOGW(TAG, "boot counter restarted: NVS was erased");
    }
    if (boot.boot != kUnknownBoot && !boot.saved) {
        ESP_LOGW(TAG, "boot counter not saved: the next boot may repeat %lu",
                 static_cast<unsigned long>(boot.boot));
    }
    const char* reason = reset_name(esp_reset_reason());
    ESP_LOGI(TAG, "boot=%lu reason=%s fw=%s", static_cast<unsigned long>(boot.boot),
             reason != nullptr ? reason : "UNKNOWN", esp_app_get_description()->version);
}

// What the board found in flash.
void log_flash_at_boot(MountResult mount, bool wanted) {
    if (!wanted) {
        ESP_LOGI(TAG, "flash: off in this run's configuration: logs are not kept");
        return;
    }
    if (mount == MountResult::Failed) {
        ESP_LOGE(TAG, "flash: mount failed: logs are not kept");
        return;
    }
    if (mount == MountResult::Formatted && !flash_store.was_blank()) {
        ESP_LOGW(TAG, "flash: FORMATTED: it could not be mounted, the previous logs are lost");
    }
    const FlashSnapshot flash = flash_store.snapshot();
    size_t used = 0;
    size_t total = 0;
    flash_store.usage(used, total);
    ESP_LOGI(TAG, "flash: %s, %lu files, %lu bytes of %lu, %s, littlefs %u/%u KB",
             mount == MountResult::Formatted ? "formatted (first use)" : to_string(mount),
             static_cast<unsigned long>(flash.store.files),
             static_cast<unsigned long>(flash.store.bytes), static_cast<unsigned long>(flash.limit),
             flash_store.tail_was_torn() ? "last line torn: closed" : "last line whole",
             static_cast<unsigned>(used / 1024), static_cast<unsigned>(total / 1024));
}

}  // namespace

void lock_tag_filter() {
    if (filter_mutex != nullptr) {
        xSemaphoreTake(filter_mutex, portMAX_DELAY);
    }
}

void unlock_tag_filter() {
    if (filter_mutex != nullptr) {
        xSemaphoreGive(filter_mutex);
    }
}

}  // namespace logcat

using namespace logcat;

extern "C" esp_err_t esp32_logcat_init(const esp32_logcat_config_t* config) {
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp32_logcat_config_t cfg =
        config != nullptr ? *config : esp32_logcat_config_t ESP32_LOGCAT_CONFIG_DEFAULT();
    filter_mutex = xSemaphoreCreateMutexStatic(&filter_mutex_storage);

    // The boot counter first (ARCHITECTURE §4): every line of this boot carries it. NVS is
    // initialized here for the whole firmware.
    const bool nvs_ready = boot_store.begin();
    const BootInfo boot = next_boot(boot_store);
    if (!nvs_ready) {
        ESP_LOGE(TAG, "nvs init failed: no boot counter");  // before the hook: ESP-IDF format
    }
    this_boot = boot.boot;
    writer.set_boot(boot.boot);

    // Sinks, writer task, then the hook: from there on every ESP_LOGx goes through the core.
    // Anything logged before (the driver installs below) keeps the ESP-IDF format, on UART.
    const bool usb_ready = cfg.usb_serial_jtag && usb_sink.begin() == ESP_OK;
    if (usb_ready) {
        writer.add_sink(usb_log);
    } else if (cfg.usb_serial_jtag) {
        ESP_LOGE(TAG, "usb_serial_jtag driver install failed: logs only on the console");
    }
    if (cfg.uart_mirror) {
        if (console_sink.begin() == ESP_OK) {
            writer.add_sink(uart_mirror);
        } else {
            ESP_LOGE(TAG, "uart driver install failed: no console mirror");
        }
    }
    writer.add_sink(ram_tail);
    // Mounted before the writer task, which is the store's only user from then on. LittleFS's
    // own logs come from inside the writer task, where they go to the ROM printer (UART) at
    // ~7 ms a line: only warnings and errors, even after `log level * V`.
    MountResult mount = MountResult::Failed;
    if (cfg.flash) {
        esp_log_level_set("esp_littlefs", ESP_LOG_WARN);
        mount = flash_store.begin();
        if (mount != MountResult::Failed) {
            writer.add_sink(flash_store.sink());
            reply_server.attach_archive(flash_store);  // `log dump` (#29); without it: unsupported
        }
    }
    log_queue.begin();
    reply_queue.begin();
    if (start_writer_task(writer, log_queue, reply_server, reply_queue,
                          [](bool queue_empty) { flash_store.on_idle(queue_empty); }) == nullptr) {
        ESP_LOGE(TAG, "writer task not created: logs stay in the ESP-IDF format");
        return ESP_ERR_NO_MEM;
    }
    install_idf_log_hook(sequencer, tag_filter, log_queue, boot.boot);
    log_boot(boot, boot_store.erased());
    log_flash_at_boot(mount, cfg.flash);

    if (cfg.console && usb_ready && start_console_task(console) == nullptr) {
        ESP_LOGE(TAG, "console task not created");
    }
    return ESP_OK;
}

extern "C" esp_err_t esp32_logcat_set_level(const char* tag, esp_log_level_t level) {
    if (!started.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tag == nullptr || tag[0] == '\0' || level < ESP_LOG_NONE || level > ESP_LOG_VERBOSE) {
        return ESP_ERR_INVALID_ARG;
    }
    const LogLevel ours = static_cast<LogLevel>(level);  // same numbers (log_record.hpp)
    lock_tag_filter();
    const bool set = tag_filter.set(tag, ours);
    if (set) {
        // The same as the console's `log level` (console.cpp): ESP-IDF's own gate too, and
        // after "*" (which wipes ESP-IDF's per-tag levels) every tag's level again.
        console_platform.set_log_level(tag, ours);
        if (tag[0] == '*' && tag[1] == '\0') {
            for (size_t i = 0; i < tag_filter.size(); ++i) {
                console_platform.set_log_level(tag_filter.tag_at(i), tag_filter.level_at(i));
            }
        }
    }
    unlock_tag_filter();
    return set ? ESP_OK : ESP_ERR_NO_MEM;
}

extern "C" esp_err_t esp32_logcat_sync(uint32_t timeout_ms) {
    if (!started.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (flash_store.snapshot().mount == MountResult::Failed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return flash_store.request_sync(pdMS_TO_TICKS(timeout_ms)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

extern "C" void esp32_logcat_set_uart_mirror(bool enabled) {
    uart_mirror.set_enabled(enabled);
}

extern "C" esp_err_t esp32_logcat_get_info(esp32_logcat_info_t* info) {
    if (info == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!started.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    const CaptureStats capture = idf_log_hook_stats();
    const FlashSnapshot flash = flash_store.snapshot();
    const TaskHandle_t writer_task = writer_task_handle();
    *info = esp32_logcat_info_t{};
    info->boot = this_boot == kUnknownBoot ? 0 : this_boot;
    info->flash_mounted = flash.mount != MountResult::Failed;
    info->dropped = capture.dropped_total;
    info->capture_cycles_last = capture.last_cycles;
    info->capture_cycles_max = capture.max_cycles;
    info->writer_stack_free = writer_task != nullptr
                                  ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(writer_task))
                                  : 0;
    info->flash_files = flash.store.files;
    info->flash_bytes = flash.store.bytes;
    return ESP_OK;
}
