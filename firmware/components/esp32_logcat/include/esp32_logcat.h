/*
 * esp32-logcat: persistent, adb-logcat-style logging for ESP-IDF firmware.
 *
 * After esp32_logcat_init(), every ESP_LOGx line of the firmware goes through the logcat:
 * numbered (boot:seq), timestamped, sent on the native USB port (USB-Serial-JTAG), mirrored on
 * the UART console and kept in flash (LittleFS on the `logs` partition), with a console that
 * answers `info`, `stats`, `log level`, `log dump`, `log sync`... (read with the esp-logcat
 * host tool). Logging never blocks the caller: a full queue drops lines and counts them.
 *
 * The project must provide some sdkconfig options and the `logs` partition: see the
 * component's README.md.
 *
 * C API (C or C++). Thread safety: esp32_logcat_init() once, from app_main (not from a static
 * constructor or an ISR), before the other calls; the others from any task after it returns
 * (before that they answer ESP_ERR_INVALID_STATE). None of them from an ISR.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log_level.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What esp32_logcat_init() turns on. Start from ESP32_LOGCAT_CONFIG_DEFAULT(). */
typedef struct {
    bool usb_serial_jtag; /* the log stream on the native USB port */
    bool uart_mirror;     /* a copy of every line on the UART console (where ROM and bootloader print) */
    bool flash;           /* keep the log in flash: LittleFS on the `logs` partition */
    bool console;         /* answer commands on the native USB port (needs usb_serial_jtag) */
} esp32_logcat_config_t;

#define ESP32_LOGCAT_CONFIG_DEFAULT() \
    { .usb_serial_jtag = true, .uart_mirror = true, .flash = true, .console = true }

/*
 * Starts the logcat: boot counter (NVS, initialized here for the whole firmware), outputs, the
 * log in flash, the writer task, the ESP_LOGx hook, then the console. Logs who this boot is
 * (`I LOGCAT: boot=N reason=... fw=...`) and what it found in flash.
 *
 * It takes over: the NVS init (an NVS partition with no free pages or a newer format is ERASED,
 * the documented recovery: other keys in it go too), the USB-Serial-JTAG and UART0 drivers
 * (stdout moves to the UART driver), and a LittleFS mounted at /logs on the `logs` partition.
 *
 * config: NULL for the defaults.
 * Returns ESP_OK; ESP_ERR_INVALID_STATE if called twice; ESP_ERR_NO_MEM if the writer task
 * could not start (then lines keep the plain ESP-IDF format). An output or the flash that
 * fails does not stop the rest: it is reported as an `E LOGCAT` line.
 */
esp_err_t esp32_logcat_init(const esp32_logcat_config_t *config);

/*
 * Sets the level of a tag at run time, like the console's `log level`: lines above it are
 * dropped before they cost anything. tag "*" sets the default; ESP_LOG_NONE silences the tag.
 * Returns ESP_ERR_INVALID_ARG (NULL or empty tag, bad level), ESP_ERR_NO_MEM (16 tags already
 * set; existing tags can always change), ESP_ERR_INVALID_STATE (before init).
 */
esp_err_t esp32_logcat_set_level(const char *tag, esp_log_level_t level);

/*
 * Makes the log in flash durable now (the console's `log sync`): returns once every line
 * logged before the call is synced, or after timeout_ms. ESP_OK, ESP_ERR_TIMEOUT (or the
 * sync failed: lines may not be in flash), ESP_ERR_NOT_SUPPORTED (no flash in this run),
 * ESP_ERR_INVALID_STATE (before init).
 */
esp_err_t esp32_logcat_sync(uint32_t timeout_ms);

/* Turns the UART mirror on or off (it drains ~70 lines/s; a load test turns it off). No effect
 * if esp32_logcat_init() ran with uart_mirror = false. */
void esp32_logcat_set_uart_mirror(bool enabled);

/* A snapshot of the logcat's counters (the console's `info`/`stats` give more). */
typedef struct {
    uint32_t boot;                /* boot counter, 1..9999; 0 = unknown */
    bool flash_mounted;           /* the log in flash is kept in this run */
    uint32_t dropped;             /* lines refused by a full queue, since boot */
    uint32_t capture_cycles_last; /* cost of the ESP_LOGx hook in the caller, CPU cycles */
    uint32_t capture_cycles_max;
    uint32_t writer_stack_free;   /* bytes the writer task never used */
    uint32_t flash_files;         /* log files in flash */
    uint32_t flash_bytes;         /* and their total size */
} esp32_logcat_info_t;

/* ESP_OK; ESP_ERR_INVALID_ARG (NULL); ESP_ERR_INVALID_STATE (before init). */
esp_err_t esp32_logcat_get_info(esp32_logcat_info_t *info);

#ifdef __cplusplus
}
#endif
