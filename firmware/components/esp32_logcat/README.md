# esp32_logcat

Persistent, `adb logcat`-style logging for ESP-IDF firmware. Add the component, call one
function, and every `ESP_LOGx` line of the firmware:

- gets a boot counter and a sequence number (`0042:000137 0000123456 W PRESSURE: out of range`),
  so a gap or a reboot shows on the PC;
- streams on the native USB port (USB-Serial-JTAG) and is mirrored on the UART console;
- is kept in flash (LittleFS, rotating files in a `logs` partition), and survives a reset;
- can be filtered per tag at run time, and dumped, synced or cleared from a console on the
  same USB port.

Logging never blocks the caller: lines go through a queue to a writer task, and a full queue
drops lines and counts them. Read it all on the PC with the `esp-logcat` host tool (this
repository's `host/`).

Targets: ESP32-C3 (ESP-IDF 6.1.1). More chips come in v2.

## Use

```c
#include "esp32_logcat.h"
#include "esp_log.h"

void app_main(void)
{
    esp32_logcat_init(NULL);                      /* defaults: USB, UART mirror, flash, console */
    esp32_logcat_set_level("CHATTY", ESP_LOG_WARN);
    ESP_LOGI("APP", "hello");                     /* from here on, every ESP_LOGx line */
}
```

A complete project: [`examples/c_minimal`](../../examples/c_minimal) (C). The demo in
[`examples/demo`](../../examples/demo) (C++) shows a queue that overflows, the heartbeat and the
timing measurement.

| Call | What it does |
|------|--------------|
| `esp32_logcat_init(config)` | Starts everything; call once, early in `app_main`. `NULL` = `ESP32_LOGCAT_CONFIG_DEFAULT()` |
| `esp32_logcat_set_level(tag, level)` | A tag's level at run time; `"*"` = default; `ESP_LOG_NONE` silences |
| `esp32_logcat_sync(timeout_ms)` | Makes the log in flash durable now |
| `esp32_logcat_set_uart_mirror(enabled)` | Turns the UART copy on or off (it drains ~70 lines/s) |
| `esp32_logcat_get_info(&info)` | Boot number, dropped lines, hook cost, writer stack, flash files |

`esp32_logcat_config_t` turns off the USB stream, the UART mirror, the flash or the console.

## What your project needs

**sdkconfig** (as in `examples/c_minimal/sdkconfig.defaults`):

| Option | Why |
|--------|-----|
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` | The native USB port carries the logcat stream; an ESP-IDF secondary console there would mix bytes into it. Build error without it |
| `CONFIG_SPI_FLASH_ENABLE_COUNTERS=y` | `stats` reports the flash sectors erased since boot. Build error without it |
| `CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y` | `log level <tag> D` can only open debug lines that were compiled in. Without it, nothing fails: the command just shows nothing |
| `CONFIG_SPI_FLASH_AUTO_SUSPEND=y` | Flash writes pause instead of stalling the CPU for a sector erase (20–25 ms stalls without it). **Check your flash chip supports suspend**: if not, the firmware asserts at boot |
| `# CONFIG_LITTLEFS_USE_MTIME is not set` | LittleFS's default (`y`) writes a modification time on every sync: flash wear for nothing, the lines carry their own time. The on-board tests run without it |

ESP-IDF 6.1.1's defaults must stay as they are for: `CONFIG_LOG_VERSION_1` and
`CONFIG_LOG_TIMESTAMP_SOURCE_RTOS` (the hook reads that format; build error otherwise), and
per-tag log levels (`CONFIG_LOG_TAG_LEVEL_IMPL_NONE` must not be set: build error otherwise, as
silencing one tag would silence the whole firmware).

**A `logs` partition** (LittleFS) of **0x80000 (512 KB)**:

```
# Name,   Type, SubType,  Offset,  Size
logs,     data, littlefs, ,        0x80000
```

The label and the budget are fixed: the log keeps up to 448 KB in 64 KB files, the oldest go
first, and the rest is LittleFS's own room. A smaller partition fills up before the rotation
frees anything; a bigger one is not used beyond 448 KB.

## What `esp32_logcat_init` takes over

- **NVS**: it initializes the NVS partition for the whole firmware (the boot counter lives
  there). If the partition has no free pages or a newer format, it is **erased** (ESP-IDF's
  documented recovery), and every other key in it goes too.
- **USB-Serial-JTAG and UART0 drivers**: installed; stdout moves to the UART driver.
- **`/logs`**: LittleFS mounted there, on the `logs` partition.
- **Log levels**: `esp_log_level_set` follows `esp32_logcat_set_level` and the console's
  `log level`; LittleFS's own logs are kept at W at most (they come from the writer task).

**Dependencies**: `joltwallet/littlefs` 1.23.1, fetched by the component manager. Pinned
exactly: it is the version the on-board tests pass with.

## Inside

C++17 without exceptions or RTTI, no heap on the logging path, everything statically
allocated. A platform-independent core (`core/`, tested on the PC) behind ports, and ESP-IDF
adapters (`port/`); only `include/esp32_logcat.h` is public. Design notes:
`docs/context/ARCHITECTURE.md` in the repository; the wire format: `docs/context/spec/wire-format.md`.
