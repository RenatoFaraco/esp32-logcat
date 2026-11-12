#pragma once

#include "adapters/log_queue.hpp"
#include "adapters/reply_queue.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logcat/console.hpp"
#include "logcat/ram_tail.hpp"
#include "logcat/reply_server.hpp"
#include "logcat/writer.hpp"

namespace logcat {

// The ESP-IDF side of the console: info and stats fields, the reply queue, reboot and crash.
class EspConsolePlatform final : public ConsolePlatform {
public:
    EspConsolePlatform(FreeRtosReplyQueue& replies, FreeRtosLogQueue& log_queue, Writer& writer,
                       ReplyServer& server, RamTail& tail)
        : replies_(replies), log_queue_(log_queue), writer_(writer), server_(server), tail_(tail) {}

    bool send(const ReplyMessage& message) override;
    void fill_info(ReplyFields& fields) override;
    void fill_stats(ReplyFields& fields) override;
    void set_log_level(const char* tag, LogLevel level) override;
    void reboot() override;
    void crash_panic() override;
    void crash_wdt() override;

private:
    FreeRtosReplyQueue& replies_;
    FreeRtosLogQueue& log_queue_;
    Writer& writer_;
    ReplyServer& server_;
    RamTail& tail_;
    uint32_t queue_timeouts_ = 0;  // only the console task touches it
};

// Reads the native USB port and runs the console. The driver must be installed (UsbJtagSink).
//
// Statically allocated. Priority 1, like the writer task: a command is never more urgent than
// acquisition (PRD §5). It blocks on the USB read, so it costs nothing while nobody types.
//
// Sole writer of the TagLevelFilter (see tag_level_filter.hpp): after this task starts, nothing
// else may add tags.
TaskHandle_t start_console_task(Console& console);

}  // namespace logcat
