# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Core `FileSystem` port (#22): append to one open file, `sync` as the only durability
  point, read by offset, size, remove, rename and list (through a visitor, no heap). The
  contract is the strict side of LittleFS, so what works on the fake works on the board: a
  size or read of the open file sees only synced bytes, a short write puts the file in error
  until it is reopened, the open file cannot be removed or renamed, and rename never replaces
  a file. The in-memory fake for the native tests (`test/support/memory_fs.hpp`) also models
  LittleFS-like blocks, failures on demand, and power cuts that roll every file back to its
  last sync (optionally keeping later bytes, for defensive tests).
- `esp-logcat shell` (#30, PRD H4): `esp-logcat shell info` runs one command and exits 0 or 1,
  `--json` prints the reply as an object (digits become numbers); without a command it opens a
  prompt where log lines print above the line being typed, filtered like `stream`; from a pipe
  it runs one command per line. The shell numbers the requests (rotating from a random start),
  waits 2 s for a reply, counts a restart as the success of `reboot` and `crash`, and shows
  reply lines nobody asked for as raw. `log tail` blocks are read by byte count and their
  CRC-32 checked (`framing.py`, reusable by `dump`). New dependency: `prompt_toolkit`.
- The `stream` summary counts console reply lines (`N replies`).

## [0.3.0] - 2026-11-11

Milestones **M2 · Capture & write** and **M3 · Host tool**, plus the device side of the
**M5 · Console**. Every `ESP_LOGx` on the board now goes through the logcat core without ever
blocking whoever logs, out on the native USB port and mirrored on the UART; `esp-logcat` reads it
live or from a file, with filters, sessions and an interactive TUI; and the board answers console
commands (`info`, `stats`, `log level`, `log tail`, `reboot`, `crash`). The external timing
measurement with the logic analyzer (#14) moved to M6; the internal one (#13) is here. Still to
come in M5: `esp-logcat dump` (#29) and `esp-logcat shell` (#30).

### Added

- Device console on the native USB port (#28): `info`, `stats`, `log level`, `log tail`,
  `reboot` and `crash <panic|wdt>` work on the board; `log dump` and `log clear` answer
  `unsupported` until flash storage (M4). Replies have their own queue and only the writer
  task writes them, alternating with the log queue. `log tail` serves the last 32 lines from
  RAM as a byte-counted block with CRC-32, with the USB out of the log meanwhile. D and V lines
  are compiled in, so `log level <tag> D` opens a tag at run time. Manual section 8 (en/pt)
  has every command with real output; fields are in wire-format §2.4.
- Core `CommandParser` (#27): `parse_command` turns a console request
  (`<id> <command> [args]`) into a `Command`: `info`, `stats`, `log dump`, `log tail`,
  `log clear`, `log level <tag> <E|W|I|D|V|N>`, `reboot`, `crash <panic|wdt>`. A request is
  understood whole or rejected with `bad_command`/`bad_args`, and the id to echo is always
  available. Accepts the line endings serial terminals send. 100 shared cases in
  `conformance/request_cases.tsv`; the grammar is in wire-format §2.1.
- Core `ReplyFormatter` (#27): builds the ADR-012 reply lines (`#>`, `#.`, `#!`, `#BEGIN`,
  `#END`) in a fixed buffer and refuses to build a malformed one; `crc32_update` (CRC-32
  IEEE, 16-entry table) for dump blocks. Checked against the same
  `conformance/reply_cases.tsv` the host parser reads.
- ADR-012, console reply framing (#69): requests carry an id (`1`–`9999`, or `-`) that every
  reply line echoes; data lines `#> <id> key=value ...`, exactly one terminator
  (`#. <id> ok` or `#! <id> <code> [text]`), dump blocks `#BEGIN <id> ...`/`#END <id> ...`,
  `proto=1` in `info`, unknown fields omitted (never zero), replies on their own queue ahead
  of the log queue. The host parser follows it exactly (`CommandReply` with fields,
  `CommandEnd`); a malformed protocol line is raw. Shared cases in
  `conformance/reply_cases.tsv` (78, plus CRC-32 check values for dump block content),
  generated from hand-written expectations by `conformance/make_reply_cases.py`.

- Timing demo (#13, PRD §7 criterion 3): a 1 kHz critical task woken by a hardware timer
  (gptimer ISR toggles GPIO 4, the task toggles GPIO 5) measures, every period, the latency
  from the alarm to the ISR and to the task (`LatencyStats`, a native-tested histogram in
  `lib/logcat_demo`). A sweep task loops through 10 s phases (no log; 100, 500 and 1000
  lines/s from another task; 1000 lines/s from the critical task itself; 1000 lines/s with the
  UART mirror on), marks each phase with pulses on GPIO 6 and logs `TIMING` lines with p50,
  p99, max, overruns, dropped lines and the critical task's stack. The UART mirror is now
  switchable and off during the sweep.

- `esp-logcat tui` (PRD H7, Textual): scroll, pause/follow, live filter that redraws
  the history, PC time column, port state in the header, counters and rate in a status
  line. Same backend as `stream` (`pipeline.events` in a worker thread, UI updated in
  50 ms batches); `--replay` and `--save` too. Manual section 3.11 (en/pt).
- A device restart while the port is closed now counts in `restarts` (`stream` and `tui`):
  the reset button on the native USB drops the connection and the ROM banner is gone before
  the host reopens it, so the first line after reconnecting is compared with the last one
  before (boot changed, uptime or seq went back).

- `--save FILE` on `stream` and `replay` (PRD H6): the whole session, unfiltered, each
  line with the PC date and time, plus the transport notices, in a readable text file
  (`session.py`). Replaying it shows exactly what was seen live; `replay --time` shows
  the saved time. Manual section 3.10 (en/pt).

- Shared line-format cases (`conformance/line_cases.tsv`, generated from hand-written
  expectations by `conformance/make_line_cases.py`): the C++ parser and formatter
  (`test_conformance`) and the Python parser (`test_conformance.py`) are checked against the same
  87 cases, so the two sides cannot drift apart; the Go host (ADR-010) will read it too.

- `esp-logcat replay FILE [filters]`: a saved log through the same pipeline as `stream` (colors,
  filters, gap and restart notices, summary), with `-` for stdin and `--lenient` for hand-edited
  files. `FileTransport` reads files saved by any shell (UTF-16 from PowerShell 5.1's `>`, with or
  without BOM, UTF-8 with or without BOM) and files saved with `stream --time`; a read error ends
  with a notice and exit status 1. New fixture: a real capture with two boots
  (`two_boots_esp32c3.log`). Manual section 3.9 (en/pt).
- `stream` and `replay` accept filters and options in any order.

- Host filters in `adb logcat` syntax: `esp-logcat stream "*:W" "PRESSURE:D"` (`filters.py`).
  `TAG:L` with levels `V D I W E` and `S`/`N` to silence, `*:L` for every other tag, the exact
  tag beats `*`. Lines without a tag always pass. Display only: gaps and the summary still count
  every line, and the summary says how many were hidden. Manual section 3.8 (en/pt).

- User manual in English (`docs/project/MANUAL.en.md`) and Portuguese (`docs/project/MANUAL.md`): a "start
  here" for PowerShell and cmd, from zero to the logcat running, how to read the output, what
  happens in each situation, pipes and files in both shells, both ports, troubleshooting. Its
  terminal pictures are real board sessions (`scripts/record_session.py`) rendered by the same
  code as `esp-logcat stream` (`scripts/manual_screenshots.py`), so they can be regenerated.
- README cover (`docs/project/img/cover.svg`, `scripts/readme_cover.py`): the board, real logcat output
  and the stack; README links both manuals and has a quick start for PowerShell and cmd.

- Firmware log pipeline: callers never wait for I/O (ADR-004). The hook only formats the
  message, filters, numbers and enqueues with zero timeout; a full queue drops and counts.
  - Core `Writer` (native-tested) with the `Sink` and `Clock` interfaces: formats each record
    once, writes it to every sink and reports losses as `W LOGCAT` lines with their own seq
    (`dropped N lines: log queue full`, `N lines not sent to usb: no reader`).
  - Adapters: static `FreeRtosLogQueue` (32 records), static writer task, `UsbJtagSink`
    (USB-Serial-JTAG driver, zero timeout, whole lines, `\n` only), `ConsoleSink` (UART mirror),
    `EspClock`. The ESP-IDF secondary console on the native port is off.
  - Measured on the board: a burst of 100 lines delivers 30 and reports the 70 dropped, matching
    the host's gaps; with no host reading the native port the heartbeat keeps its 1000 ms period
    (it was 1050 ms with the synchronous output).

- Firmware `IdfLogHook`: every `ESP_LOGx` (application and ESP-IDF) goes through the core —
  recognized by the new core `parse_idf_text`, filtered per tag, numbered and written in the
  format of wire-format §1. Output is still synchronous in the caller's context (queue and writer task
  come in #11/#12). Demo app with a 1 s heartbeat showing capture cost and stack headroom.
- Host `parser.py`: classifies each line as an esp32-logcat `LogLine` (validated exactly like the
  firmware parser, same rejection reasons), `InvalidLine`, ESP-IDF native `IdfLine`,
  `CommandReply`, dump markers or `RawLine`. Pure functions; never raises on any input. Tested
  against a real boot capture from the board (CRLF line endings included).
- Host `esp-logcat stream` (no filters yet) and `esp-logcat ports`. Device lines go to stdout
  exactly as written (pipe and grep friendly); notices and a final summary go to stderr.
  - `transport.py`: opens with DTR/RTS released, resets with the DTR rewrite usbser.sys
    needs, normalizes CRLF, never wraps or grows without bound (1024-byte line cap), waits
    for an absent or busy port and reconnects, finding the board again under a new COM
    number (serial number on the native port, USB location on the CH340K).
  - `stats.py`: seq continuity per connection — lines lost (undone if the line arrives late,
    within the queue depth), reordered, device restarts (also via the `ESP-ROM:` banner, so a
    boot loop is visible while boot stays 0), uptime or seq going back (a corrupted line is
    never ~1e6 lost lines), plus counters per line kind and invalid reason.
  - Ends quietly when the reader of a pipe goes away (`stream | head`), closes the port on
    every path (including Ctrl+C during a reset, which also releases RTS), and reports a busy
    port in any Windows language.
  - `pipeline.py`: bytes → lines → parser → stats, shared with the future replay and TUI.
    Splits the 64-byte fragment the native port keeps in its FIFO, which otherwise glues onto
    the first live line as a false but valid-looking line (docs/context/hardware.md, item 7).
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

### Changed

- `docs/` split by audience: `docs/project/` holds how we work (manuals, conventions, roadmap,
  pictures) and `docs/context/` holds what the system is and why (PRD, architecture, hardware,
  ADRs). `docs/README.md` is the index. Files moved unchanged; every path that cited them
  (README, CLAUDE.md, agents, templates, scripts, code comments) follows.
- One PRD per version in `docs/context/prd/`: `v1.md` (the current one, numbering kept) and
  drafts for v2 (multichip, ADR-009), v2.x (Go host, ADR-010) and v3 (Rust crate, ADR-011, and
  MQTT telemetry), built only from what the ADRs and the ROADMAP already decide.
- The wire contract moves to `docs/context/spec/wire-format.md`: the log line (§1, the former
  PRD §3, unchanged), console replies (§2) and dump blocks (§3) from ADR-012, and the host
  session file (§4). Citations of "PRD §3" in code, tests and docs now say "wire-format §1"
  (except `scripts/github_bootstrap.py`, which mirrors the text of issues already published).

### Fixed

- UART mirror no longer waits (#28): it wrote through stdout, one byte per driver call, and
  the driver busy-waits on a full ring. Under load the writer task could stay stuck there for
  good. Now a line goes in whole if it fits, or is refused and counted. In the UART phase of
  the timing sweep the timer ISR's worst delay went from 99 ms to about 0.1 ms.
- `test_replay_colors_on_a_terminal` no longer fails in a terminal with `NO_COLOR` set (rich
  honors it even on a forced terminal).
- SECURITY: signing and hash chaining of logs are in the backlog with no version, as the PRD
  and the ROADMAP say, not "planned for v3".

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

[Unreleased]: https://github.com/RenatoFaraco/esp32-logcat/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/RenatoFaraco/esp32-logcat/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/RenatoFaraco/esp32-logcat/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/RenatoFaraco/esp32-logcat/releases/tag/v0.1.0
