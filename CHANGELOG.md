# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Firmware `IdfLogHook`: every `ESP_LOGx` (application and ESP-IDF) goes through the core —
  recognized by the new core `parse_idf_text`, filtered per tag, numbered and written in the
  PRD §3 format. Output is still synchronous in the caller's context (queue and writer task
  come in #11/#12). Demo app with a 1 s heartbeat showing capture cost and stack headroom.
- Host `parser.py`: classifies each line as an esp32-logcat `LogLine` (validated exactly like the
  firmware parser, same rejection reasons), `InvalidLine`, ESP-IDF native `IdfLine`,
  `CommandReply`, dump markers or `RawLine`. Pure functions; never raises on any input. Tested
  against a real boot capture from the board (CRLF line endings included).
- Host `render.py`: each parsed line becomes a `rich.text.Text` colored by level (adb logcat
  palette, bright blue for Debug), whose plain text is the original line. ESP-IDF lines in the
  level color without bold, raw lines dimmed, invalid lines labeled with their reason and raw
  bytes, optional PC timestamp. Device text is never markup; control and format characters
  and lone surrogates are shown as ASCII escapes. Ruff now rejects bidi characters in source
  (`PLE2502`).
- ADR-008: the host is layered (transport → parser → stats/filters → render) and gets an
  interactive TUI (`esp-logcat tui`, Textual) as a second frontend; `stream` stays plain stdout.
- `host-reviewer` subagent: read-only review of host code (layers, format contract, serial on
  Windows, device text as data, TUI event loop, tests without a TTY).

## [0.2.0] - 2026-11-06

Milestone **M1 · Core**: the platform-independent logging core, tested on the PC and under
sanitizers, plus the first run on real hardware. Not yet a logging system on the device: the
esp_log hook, queue and writer task come in M2.

### Added

**Core library** (`firmware/lib/logcat_core`, C++17, no heap, no platform includes)

- `LogRecord` (160 B, trivially copyable for the FreeRTOS queue) with bounded tag/message
  copies and a truncation flag; `LogLevel` numbered like ESP-IDF's levels, with letter
  conversions for the line format and `LogLevel::None` to silence a tag.
- `Sequencer`: per-boot `seq` wrapping exactly at 1,000,000 (6-digit field) and a dropped-line
  counter, safe for concurrent loggers.
- `format_line`: `LogRecord` to the PRD §3 line (max 170 bytes) in a fixed-size buffer, no
  printf. Boot printed modulo 10000, seq modulo 1e6, non-ASCII and control bytes mapped to `?`
  (a message can never forge a line), empty tag `-`, truncated messages as 124 chars + `...`.
- `parse_line`: strict parser returning a categorized status (`ok`, `torn`, `garbage`,
  `bad_header`, `bad_level`, `overlong`). Torn lines (no final `\n`, or closed with DEL) are
  never taken as records; `format_line(parse_line(L)) == L` for every valid line.
- `RingBuffer<T, N>`: header-only circular buffer with no constructor (usable in RTC_NOINIT
  memory), a single-counter state so a reset mid-push cannot leave mismatched indices,
  `is_consistent()` for memory that survived a reset, and every access bounded even when the
  state is garbage.
- `TagLevelFilter`: per-tag threshold with a default (`*`), 16 tags compared on their first
  15 chars; lock-free readers (release/acquire publication, no read-modify-write atomics — no
  emulated `__atomic_*` calls on the ESP32-C3).
- The target build compiles the core library.

**Hardware**

- Board bring-up: ESP32-C3 rev v0.4 with 4 MB embedded flash, CH340K bridge on UART0 and
  native USB-Serial-JTAG, both receiving the console. The core formatter's first line on the
  device matches the PRD §3 format. Findings in `docs/hardware.md`.
- `scripts/serial_capture.py`: resets the board through the native USB and captures the boot
  on both ports (includes the Windows `usbser.sys` workaround for RTS).

**CI and tooling**

- Core tests under AddressSanitizer + UndefinedBehaviorSanitizer (`native_asan`) and
  ThreadSanitizer (`native_tsan`); each was proven to catch a planted bug of its kind.
- Project subagents for firmware review, persistence audit, test running and milestone checks.

**Documentation**

- ADR-006: log lines are printable ASCII only, single-line.
- ADR-007: no per-line checksum in v1, with the bit flips that still pass.
- PRD §3 states the exact line contract (field rules, truncation, torn-line sentinel).
- `SECURITY.md` with supported versions, private vulnerability reporting and known
  limitations.

## [0.1.0] - 2026-10-21

Milestone **M0 · Foundation**: project scaffolding, build environments and CI.
No logging functionality yet.

### Added

- **Firmware project** (PlatformIO, ESP-IDF 6.x via `espressif32` 7.1.3, pinned):
  - `env:esp32c3` for the target (generic ESP32-C3, DIO flash) with a "hello" app.
  - `env:native` for host unit tests with Unity: strict C++17, `-Wall -Wextra -Werror`,
    no exceptions, no RTTI, plus a toolchain test asserting the C++ standard.
  - Custom partition table: NVS, 1.5 MB factory app, 512 KB `logs` (LittleFS), 64 KB core dump.
  - `sdkconfig.defaults` with flash size, custom partitions and C++ exceptions/RTTI disabled.
- **Host tool** `esp-logcat` (Python 3.12): package skeleton with the `stream`, `dump`, `shell`
  and `replay` subcommands registered as placeholders, ruff and pytest configured.
- **CI** (GitHub Actions): native tests, target build, C++ formatting check (clang-format),
  ruff, pytest, and schema validation of issue forms and workflows.
- **Docs**: PRD, architecture, roadmap, conventions and ADRs 001–005 (ESP-IDF via PlatformIO,
  text line format, platform-independent core, drop on full queue, strict C++17 on the core).
- **Contribution tooling**: issue forms (bug, crash/unexpected reset, feature, performance,
  documentation and planned-work tasks), pull request template, and
  `scripts/github_bootstrap.py` recording the M0–M7 plan as labels, milestones and issues.
- **Claude Code project setup**: subagents for firmware review, persistence audit, test running
  and milestone checks, with pre-approved test commands.
- Repository basics: MIT license, `.gitignore`, `.gitattributes` (LF everywhere),
  `.clang-format`.

[Unreleased]: https://github.com/RenatoFaraco/esp32-logcat/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/RenatoFaraco/esp32-logcat/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/RenatoFaraco/esp32-logcat/releases/tag/v0.1.0
