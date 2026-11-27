#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/console.hpp"
#include "logcat/ram_tail.hpp"
#include "logcat/reply_server.hpp"
#include "logcat/writer.hpp"
#include "port/flash_store.hpp"
#include "port/log_queue.hpp"
#include "port/reply_queue.hpp"

namespace logcat {

// The ESP-IDF side of the console: info and stats fields, the reply queue, reboot and crash.
class EspConsolePlatform final : public ConsolePlatform {
public:
    EspConsolePlatform(FreeRtosReplyQueue& replies, FreeRtosLogQueue& log_queue, Writer& writer,
                       ReplyServer& server, RamTail& tail, FlashStore& flash)
        : replies_(replies),
          log_queue_(log_queue),
          writer_(writer),
          server_(server),
          tail_(tail),
          flash_(flash) {}

    bool send(const ReplyMessage& message) override;
    void fill_info(ReplyFields& fields) override;
    void fill_stats(ReplyFields& fields) override;
    void set_log_level(const char* tag, LogLevel level) override;
    void reboot() override;
    void crash_panic() override;
    void crash_wdt() override;
    SyncResult sync_log() override;
    ClearResult clear_log() override;
    void lock_filter() override;  // shared with esp32_logcat_set_level() (port/filter_lock.hpp)
    void unlock_filter() override;

private:
    FreeRtosReplyQueue& replies_;
    FreeRtosLogQueue& log_queue_;
    Writer& writer_;
    ReplyServer& server_;
    RamTail& tail_;
    FlashStore& flash_;
    uint32_t queue_timeouts_ = 0;  // only the console task touches it
};

// Reads the native USB port and runs the console. The driver must be installed (UsbJtagSink).
//
// Statically allocated. Priority 1, like the writer task: a command is never more urgent than
// acquisition (PRD §5). It blocks on the USB read, so it costs nothing while nobody types.
//
// Writes the TagLevelFilter under the filter lock (port/filter_lock.hpp), like
// esp32_logcat_set_level(): new tags are added by one writer at a time.
TaskHandle_t start_console_task(Console& console);

}  // namespace logcat
