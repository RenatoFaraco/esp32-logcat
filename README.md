# esp32-logcat

Persistent, `adb logcat`-style logging for embedded firmware.

> **Status:** early development (milestone M0 — project foundation). Nothing here is usable yet.

## What it is

Devices in the field crash, reboot or misbehave, and by the time they reach the bench there is no
record of what happened. `printf` over serial only helps while the cable is plugged in, vanishes on
reset and never tells you *why* the board restarted.

esp32-logcat is a reusable logging component plus a PC tool that:

- captures every `ESP_LOGx` call (application and ESP-IDF itself) without a new API;
- never blocks the caller — logging only enqueues, a low-priority task formats and writes;
- persists logs to a dedicated LittleFS partition with file rotation and a size cap;
- records boot count, reset reason and firmware version on every boot;
- keeps a "last words" buffer in RTC memory to recover what happened right before a panic or
  watchdog reset;
- ships `esp-logcat`, a Python CLI to stream, filter (`"*:W" "PRESSURE:D"`), dump and replay logs.

Log line format:

```
<boot>:<seq> <uptime_ms> <L> <TAG>: <message>
0042:000137 0000123456 W PRESSURE: reading out of range (4.71 V)
```

## Target

- ESP32-C3 (RISC-V), ESP-IDF via PlatformIO, C++17.
- Platform-independent core (`firmware/lib/logcat_core`) unit-tested on the host (`env:native`).
- Host tool: Python 3.12.

## Repository layout

```
firmware/   PlatformIO project (core library, ESP-IDF adapters, demo app, tests)
host/       esp-logcat Python CLI
docs/       PRD, architecture, roadmap, conventions and ADRs (Portuguese)
```

## Building and testing

_Coming in M0._

## License

[MIT](LICENSE)
