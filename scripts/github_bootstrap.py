"""Bootstrap GitHub labels, milestones and issues for esp32-logcat from the PRD/ROADMAP.

Records the initial project plan as data. Idempotent: labels, milestones and issues are matched
by name/title and only created when missing; labels listed here are added to existing issues.

Usage (requires an authenticated GitHub CLI, `gh auth login`):
    python scripts/github_bootstrap.py           # dry run
    python scripts/github_bootstrap.py --apply
"""

# ruff: noqa: E501  (issue text is data; long lines read better than split strings)
import json
import shutil
import subprocess
import sys

GH = shutil.which("gh") or r"C:\Program Files\GitHub CLI\gh.exe"
REPO = "RenatoFaraco/esp32-logcat"
APPLY = "--apply" in sys.argv

LABELS = [
    ("core", "Platform-independent C++ core (firmware/lib/logcat_core)", "1d76db"),
    ("fw", "ESP-IDF adapters, demo app, target build", "0e8a16"),
    ("host", "esp-logcat host tool (Python, then Rust)", "fbca04"),
    ("rust", "Rust code (host-rs workspace)", "dea584"),
    ("ci", "GitHub Actions, build and tooling", "5319e7"),
    ("needs-hardware", "Requires the ESP32-C3 board (expected ~2026-11-11)", "b60205"),
    ("test", "Test or measurement task, including acceptance criteria", "c2e0c6"),
    ("perf", "Timing, memory or throughput", "f9d0c4"),
    ("release", "Release preparation", "0052cc"),
]

FOUNDATION_PRS = (1, 2, 3)  # merged before milestones existed; attached to M0

MILESTONES = [
    (
        "M0 · Foundation",
        "Repository, PlatformIO envs (esp32c3 + native), host skeleton, CI green.",
        "closed",
    ),
    ("M1 · Core", "LogRecord, formatter, parser, ring buffer, tag filter — native tests.", "open"),
    (
        "M2 · Capture & write",
        "esp_log hook, non-blocking queue, writer task, USB output, timing impact measured.",
        "open",
    ),
    ("M3 · Host tool", "Parser, filters, colors and replay — works without hardware.", "open"),
    ("M4 · Persistence", "LittleFS with rotation, boot record, flush policy.", "open"),
    ("M5 · Console", "Device console commands, esp-logcat dump and shell.", "open"),
    (
        "M6 · Robustness",
        "Last words in RTC memory, crash and power-cut tests, v1.0.0 release.",
        "open",
    ),
    ("M7 · Core dump (stretch)", "Core dump to flash and analysis instructions.", "open"),
    (
        "M5.5 · Toolchain",
        "Firmware on plain ESP-IDF (ADR-013) and the host in Rust (ADR-014), v0.5.0.",
        "open",
    ),
    (
        "Multi-board host",
        "The Rust host reads any ESP board running any ESP-IDF firmware, several boards at once.",
        "open",
    ),
    (
        "v2 · Multichip component",
        "The logcat component on ESP32, S3, C6 and P4, published on the IDF Component Registry (ADR-009, PRD v2).",
        "open",
    ),
]


def body(summary, scope, done, refs):
    lines = [summary, "", "### Scope"] + [f"- {s}" for s in scope]
    lines += ["", "### Done when"] + [f"- [ ] {d}" for d in done]
    lines += ["", f"**Refs:** {refs}"]
    return "\n".join(lines)


M1, M2, M3, M4, M5, M6, M7, M55, MB, V2 = (m[0] for m in MILESTONES[1:])

ISSUES = [
    # ---------------- M1 ----------------
    (
        M1,
        ["core"],
        "core: LogRecord and Sequencer",
        body(
            "Fixed-size record that travels through the queue, plus the per-boot sequence counter.",
            [
                "`LogRecord`: `boot`, `seq`, `uptime_ms`, `level`, `tag[16]`, `msg[N]`, `truncated`",
                "`LogLevel` enum class (E/W/I/D/V)",
                "`Sequencer`: monotonic `seq` per boot and `dropped` counter",
                "Size budget ~160 B (ARCHITECTURE §8); `static_assert` on size",
            ],
            [
                "Unity tests for sequencing, dropped counter and tag/msg truncation",
                "No dynamic allocation, no platform includes",
            ],
            "PRD §3 (line format), F3 · ARCHITECTURE §2.1",
        ),
    ),
    (
        M1,
        ["core"],
        "core: LineFormatter",
        body(
            "`LogRecord` → text line in the PRD §3 format.",
            [
                "`<boot>:<seq> <uptime_ms> <L> <TAG>: <msg>` with fixed-width numeric fields",
                "Truncated messages end with the `…` marker",
                "Writes into a caller-provided buffer; never allocates",
            ],
            ["Tests: nominal line, field widths/overflow, long tag, truncation, empty message"],
            "PRD §3 · ADR-002",
        ),
    ),
    (
        M1,
        ["core"],
        "core: LineParser with round-trip tests",
        body(
            "Text line → `LogRecord`. Used on the device to read flash and mirrored by the host parser.",
            [
                "Strict parse of the PRD §3 format; reports failure on malformed lines",
                "Shared example lines in `host/tests/fixtures/` used by the C++ tests (deferred to #20)",
            ],
            [
                "Round trip: `parse(format(r)) == r` for representative records",
                "Malformed inputs rejected (bad widths, missing fields, unknown level)",
            ],
            "PRD §3 · ADR-002 · CONVENTIONS §5",
        ),
    ),
    (
        M1,
        ["core"],
        "core: RingBuffer<T, N>",
        body(
            "Static circular buffer that overwrites the oldest entry — holds the last N lines in RAM.",
            [
                "Template on type and capacity; no heap",
                "push / size / iterate oldest→newest / clear",
            ],
            ["Tests: empty, partial, exactly full, wrap-around overwrites oldest"],
            "PRD F4 · ARCHITECTURE §2.1",
        ),
    ),
    (
        M1,
        ["core"],
        "core: TagLevelFilter",
        body(
            "Per-tag log level with a default level, adjustable at runtime.",
            [
                "Static table (e.g. 16 tags), no allocation",
                "`set(tag, level)`, `enabled(tag, level)`, default level, table-full behavior",
            ],
            ["Tests: default level, per-tag override, update existing tag, table full"],
            "PRD F2 · ARCHITECTURE §2.1",
        ),
    ),
    # ---------------- M2 ----------------
    (
        M2,
        ["fw", "needs-hardware"],
        "fw: board bring-up",
        body(
            "First contact with the ESP32-C3 dual USB-C boards.",
            [
                "Identify which USB-C is the native USB-Serial-JTAG and which is the USB-UART bridge (and its chip)",
                "Confirm flash size with `esptool.py flash_id`; adjust `partitions.csv`/`sdkconfig.defaults` if needed",
                "Confirm DIO vs QIO flash mode",
                "Flash the hello firmware and see output on both ports",
            ],
            ["Findings recorded in `docs/` (port mapping, flash chip, size)"],
            "PRD §6 · ROADMAP 'Chegada das placas'",
        ),
    ),
    (
        M2,
        ["fw", "needs-hardware"],
        "fw: IdfLogHook — capture every ESP_LOGx",
        body(
            "Install `esp_log_set_vprintf` to capture application and ESP-IDF logs without a new API.",
            [
                "Format the message in the caller's context into a fixed local buffer (`va_list` cannot cross tasks)",
                "Build a `LogRecord` and enqueue it; apply `TagLevelFilter`",
                "Installed as early as possible in `app_main`",
            ],
            [
                "Logs from the app and from IDF components arrive as records",
                "Caller-side cost measured (feeds the jitter measurement issue)",
            ],
            "PRD F1, F2 · ARCHITECTURE §3",
        ),
    ),
    (
        M2,
        ["fw", "needs-hardware"],
        "fw: non-blocking log queue with drop counter",
        body(
            "`FreeRtosLogQueue`: enqueue with zero timeout; never block the caller.",
            [
                "Queue full → drop and increment `dropped`",
                "Writer emits `W LOGCAT: dropped N lines` when traffic resumes",
            ],
            ["Burst test shows drops reported and `seq` gaps visible on the host"],
            "PRD F3 · ADR-004",
        ),
    ),
    (
        M2,
        ["fw", "needs-hardware"],
        "fw: WriterTask, UsbJtagSink and EspClock",
        body(
            "Low-priority task that consumes the queue, formats and writes to sinks.",
            [
                "`WriterTask` with `LineFormatter`; sinks behind the core `Sink` interface",
                "`UsbJtagSink` (usb_serial_jtag driver), `EspClock` (`esp_timer_get_time`)",
                "Reentrancy guard: logs produced inside the writer go straight to USB, bypassing the queue",
            ],
            [
                "Lines appear on the USB port in the PRD §3 format",
                "No recursion/deadlock when drivers log from inside the writer",
            ],
            "PRD F3 · ARCHITECTURE §2.3, §3",
        ),
    ),
    (
        M2,
        ["fw", "needs-hardware"],
        "fw: demo app with 1 kHz critical task",
        body(
            "Demo application that generates logs and runs a periodic 1 kHz task toggling a GPIO.",
            [
                "Configurable log rate (up to 1000 lines/s) from a separate task",
                "GPIO toggle on every critical-task period for the logic analyzer",
            ],
            ["Demo runs stably with logging on and off"],
            "PRD §7 criterion 3",
        ),
    ),
    (
        M2,
        ["fw", "perf", "test", "needs-hardware"],
        "fw: measure timing impact and define X",
        body(
            "Acceptance criterion 3: logging 1000 lines/s must not delay a 1 kHz task by more than X µs.",
            [
                "Logic analyzer (PulseView/sigrok) on the GPIO toggle",
                "Internal measurement with `esp_timer` to cross-check",
                "Compare logging off / on at increasing rates",
            ],
            ["X defined and documented with captures; queue size tuned (ADR-004)"],
            "PRD §7 criterion 3 · PRD §5",
        ),
    ),
    (
        M2,
        ["host", "needs-hardware"],
        "host: esp-logcat stream (live)",
        body(
            "Stream live logs from the device over serial, colored by level, with logcat-style filters.",
            [
                "`SerialTransport` (pyserial) reusing the M3 parser, filters and renderer",
                "Port selection and reconnect on device reset",
            ],
            ['`esp-logcat stream "*:W" "PRESSURE:D"` works against the board'],
            "PRD H1, H2",
        ),
    ),
    # ---------------- M3 ----------------
    (
        M3,
        ["host"],
        "host: line parser",
        body(
            "Pure-function parser for the wire protocol.",
            [
                "Line types: log, command reply (`#> `), dump block (`#BEGIN`/`#END`), raw",
                "`LogLine` dataclass mirroring `LogRecord`",
            ],
            ["pytest coverage for each type and malformed input"],
            "PRD §3 · ARCHITECTURE §6, §7",
        ),
    ),
    (
        M3,
        ["host"],
        "host: logcat-style filters",
        body(
            'Filter expressions like `"*:W" "PRESSURE:D"`.',
            [
                "Parse `TAG:L` specs, wildcard default, most-specific wins",
                "Raw lines and command replies always pass",
            ],
            ["pytest table of specs × lines"],
            "PRD H2",
        ),
    ),
    (
        M3,
        ["host"],
        "host: colored renderer",
        body(
            "Render parsed lines to the terminal with colors per level (rich).",
            ["Level colors, dimmed raw lines, optional PC timestamp column"],
            ["Snapshot-style tests on rendered text (no TTY needed)"],
            "PRD H1",
        ),
    ),
    (
        M3,
        ["host"],
        "host: esp-logcat replay and fixtures",
        body(
            "Replay a saved log file with the same filters and rendering — development without hardware.",
            [
                "`FileTransport`; `esp-logcat replay <file> [filters...]`",
                "Example logs in `host/tests/fixtures/` (multiple boots, drops, raw bootloader lines)",
            ],
            ["Replaying a fixture filters and colors correctly"],
            "PRD H5",
        ),
    ),
    (
        M3,
        ["host", "core", "test"],
        "test: cross-language format compatibility",
        body(
            "Guarantee the C++ formatter/parser and the Python parser agree on the same lines.",
            ["Shared fixture of valid/invalid lines consumed by Unity tests and pytest"],
            ["Both suites run against the same fixture in CI"],
            "CONVENTIONS §5 · CLAUDE.md (format change rule)",
        ),
    ),
    (
        M3,
        ["host"],
        "host: save session with PC timestamps",
        body(
            "Save a session to file with the PC timestamp next to the device uptime.",
            ["`--save <file>` on stream/replay; output re-readable by `replay`"],
            ["Round trip: saved session replays identically"],
            "PRD H6",
        ),
    ),
    # ---------------- M4 ----------------
    (
        M4,
        ["core"],
        "core: FileSystem port and in-memory fake",
        body(
            "Interface for file operations used by the store, plus a fake for native tests.",
            [
                "`open_append`, `write`, `size`, `remove`, `rename`, `list`",
                "Fake with configurable capacity and failure injection",
            ],
            ["Fake covered by its own tests"],
            "ARCHITECTURE §2.2 · ADR-003",
        ),
    ),
    (
        M4,
        ["core"],
        "core: RotatingStore",
        body(
            "Rotation logic over `FileSystem`: current file, per-file limit, total limit, discard oldest.",
            [
                "Files `log_000.txt`…`log_NNN.txt`; current = highest index",
                "64 KB per file, 448 KB total (ARCHITECTURE §5)",
            ],
            [
                "Tests: fill, rotate, discard oldest, survive restart (rescan existing files)",
                "Partition never exceeds the limit",
            ],
            "PRD F5, §7 criterion 4",
        ),
    ),
    (
        M4,
        ["fw", "needs-hardware"],
        "fw: LittleFsFileSystem adapter",
        body(
            "`FileSystem` implementation on `esp_littlefs`, partition `logs`.",
            ["Mount; format if corrupted; report mount errors once"],
            ["RotatingStore runs on the board and survives reboot"],
            "PRD F5 · ARCHITECTURE §2.3",
        ),
    ),
    (
        M4,
        ["fw", "needs-hardware"],
        "fw: boot record (NvsBootInfo)",
        body(
            "Boot counter in NVS, reset reason and firmware version, logged on every boot.",
            [
                "`boot_count++` in NVS; `esp_reset_reason()` mapped to `ResetReason`",
                "`I LOGCAT: boot=<n> reason=<...> fw=<version>`",
            ],
            ["Counter increments across resets; reasons verified for power-on and software reset"],
            "PRD F7 · ARCHITECTURE §4",
        ),
    ),
    (
        M4,
        ["fw", "needs-hardware"],
        "fw: flush policy",
        body(
            "Flush to flash periodically (5 s), immediately on ERROR, and on command.",
            ["Batched writes to limit flash wear"],
            [
                "ERROR lines are persisted once the flush completes",
                "Data-loss window documented as the flush interval",
            ],
            "PRD F6, §5, §7 criterion 2",
        ),
    ),
    # ---------------- M5 ----------------
    (
        M5,
        ["core"],
        "core: CommandParser",
        body(
            "Console command line → `Command` (enum + arguments).",
            [
                "`info`, `log dump`, `log tail`, `log clear`, `log level <tag> <level>`, `reboot`, `crash <panic|wdt>`"
            ],
            ["Tests for valid, invalid and wrong-argument commands"],
            "PRD F9",
        ),
    ),
    (
        M5,
        ["fw", "needs-hardware"],
        "fw: device console",
        body(
            "Console on the native USB port executing the parsed commands.",
            [
                "Replies prefixed with `#> `",
                "`log tail` serves the RAM ring buffer; `log level` updates `TagLevelFilter`",
            ],
            ["Every F9 command works on the board"],
            "PRD F4, F9 · ARCHITECTURE §6",
        ),
    ),
    (
        M5,
        ["fw", "needs-hardware"],
        "fw: log dump and log clear",
        body(
            "Download persisted logs from flash to a file.",
            [
                "Device emits `#BEGIN <file> <bytes>` … `#END <crc32>`",
                "Host verifies CRC and writes files",
            ],
            ["Dumped files replay correctly; CRC mismatch is reported"],
            "PRD H3 · ARCHITECTURE §6",
        ),
    ),
    (
        M5,
        ["host", "needs-hardware"],
        "host: esp-logcat shell",
        body(
            "Interactive shell that sends console commands and shows replies alongside logs.",
            [],
            ["All F9 commands usable from the shell"],
            "PRD H4",
        ),
    ),
    # ---------------- M5.5 · Toolchain ----------------
    (
        M55,
        ["fw", "ci"],
        "build: firmware on plain ESP-IDF (idf.py), component layout",
        body(
            "Move the build from PlatformIO to idf.py and give the firmware the layout of a publishable component. The code does not change language (C++17, ADR-005).",
            [
                "`firmware/components/esp32_logcat/` with `core/` (today `lib/logcat_core`, still IDF-free, ADR-003) and `port/` (today `src/adapters`)",
                "`firmware/examples/demo/`: demo app, `sdkconfig.defaults`, `partitions.csv`",
                "IDF pinned to the version the current build uses (6.1, `dependencies.lock`)",
                "Timing sweep (#13/#14) as a Kconfig option enabled by `sdkconfig.timing`, not a `-D` flag",
                "`platformio.ini` and PlatformIO scripts removed",
            ],
            [
                "`idf.py build` of the demo passes; flashed on the C3 it logs like v0.4.0 (stream, shell, flash store)",
                "No `pio` left in the repository",
            ],
            "ADR-013 §1, §2, §6",
        ),
    ),
    (
        M55,
        ["core", "ci", "test"],
        "test: core tests on CMake + Unity + ctest, with sanitizer presets",
        body(
            "Replace `env:native`, `env:native_asan` and `env:native_tsan` with a plain CMake project.",
            [
                "`firmware/test/host/` with Unity pinned by version",
                "`CMakePresets.json`: `native`, `asan` (ASan + UBSan), `tsan`",
                "ADR-005 flags kept: `-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror`",
                "`conformance/*.tsv` still read by `test_conformance`",
            ],
            [
                "Every current native test runs under ctest on Linux and Windows",
                "asan and tsan presets green in CI",
            ],
            "ADR-013 §4 · ADR-005",
        ),
    ),
    (
        M55,
        ["fw", "test", "needs-hardware"],
        "test: on-board tests as an ESP-IDF test app",
        body(
            "Port `test_target_littlefs` and `test_target_nvs` to an IDF test app with Unity.",
            ["`firmware/test/target/`, run with `idf.py -p COMx flash monitor`"],
            ["Both suites pass on the C3"],
            "ADR-013 §5",
        ),
    ),
    (
        M55,
        ["fw"],
        "fw: public C API (esp32_logcat.h)",
        body(
            "A C header at the component boundary, so C projects use the component without touching C++.",
            [
                "`esp32_logcat_init(const esp32_logcat_config_t *)` and the calls the demo uses (sync, level, info)",
                "`extern \"C\"` wrappers over the C++ adapters; no C++ type crosses the header",
            ],
            ["The demo's `app_main` uses only `esp32_logcat.h`", "A C-only example compiles"],
            "ADR-013 §3",
        ),
    ),
    (
        M55,
        ["ci", "documentation"],
        "ci: ESP-IDF container build; docs and agents without PlatformIO",
        body(
            "CI and project instructions follow the new build.",
            [
                "Firmware job on the pinned `espressif/idf` image; core jobs on CMake without IDF",
                "`CLAUDE.md`, `CONVENTIONS.md`, `CONTRIBUTING.md`, `README.md`, ADR-005, `hardware.md`, MANUAL (pt/en, build/flash section)",
                "`.claude/settings.json` and agents `test-runner`, `firmware-reviewer`, `milestone-checker`",
                "Issue templates, PR template, `.vscode/` recommendations, README cover (`scripts/readme_cover.py`)",
            ],
            [
                "CI green",
                "No build or test instruction uses `pio`: `git grep -nE 'pio (run|test)|env:native'` is empty outside ADRs and the CHANGELOG",
            ],
            "ADR-013, Consequences",
        ),
    ),
    (
        M55,
        ["host", "rust", "ci"],
        "host(rs): Cargo workspace and logcat-wire passing conformance/",
        body(
            "Start the Rust host from the part with no I/O: the wire format.",
            [
                "`host-rs/` workspace: `logcat-wire`, `logcat-serial`, `esp-logcat`",
                "`logcat-wire`: line parser and classification, `seq` gaps, reply/dump framer, session file (wire-format §1–§4)",
                "CI job: `cargo fmt --check`, `cargo clippy -D warnings`, `cargo test` on Linux and Windows",
            ],
            ["`logcat-wire` passes every case in `conformance/*.tsv`"],
            "ADR-014 §1, §2 · PRD rust-host RH-2, RH-3",
        ),
    ),
    (
        M55,
        ["host", "rust", "needs-hardware"],
        "host(rs): ports and stream (reconnect, DTR/RTS, glued lines)",
        body(
            "The serial side, with everything the Python host learned on the board.",
            [
                "`logcat-serial`: ports by VID:PID, CH340K opened with DTR/RTS released, reconnect when the COM disappears or changes",
                "Glued lines from the FIFO split as `pipeline.split_glued` does",
                "`esp-logcat ports` and `esp-logcat stream` with `TAG:L` filters, plain text on stdout",
                "When more than one port is found and `--port` is missing: list ports with description and VID:PID",
            ],
            ["`stream` on the C3 over both ports matches the Python host's output for the same session"],
            "PRD rust-host RH-1, RH-5 · hardware.md",
        ),
    ),
    (
        M55,
        ["host", "rust", "needs-hardware"],
        "host(rs): restart detection over UART and USB-Serial-JTAG",
        body(
            "Count and mark device restarts with any ESP-IDF firmware, not only ours.",
            [
                "UART: the ROM `rst:0x.. (REASON)` line starts a new boot, with the reason",
                "USB-Serial-JTAG (the ROM and bootloader never write there; the port drops on reset): port drop + reconnect, confirmed by the timestamp going backwards; `last_ts` survives the reconnect",
                "Fallback: timestamp regression with no `rst:` line",
                "Visible separator `restart #N · REASON` and the `restarts` counter",
            ],
            [
                "A reset button press is counted on the CH340K port and on the native port of the C3",
                "Replay fixtures for both cases in `conformance/`",
            ],
            "PRD rust-host RH-7 · observed on an ESP32-C6 DevKit, 2026-11-26",
        ),
    ),
    (
        M55,
        ["host", "rust"],
        "host(rs): colors, --save and replay",
        body(
            "Sessions recorded by one host replay the same on the other.",
            ["Colors by level (text from the board is data, never markup)", "`--save` with PC time (wire-format §4)", "`esp-logcat replay`"],
            ["A session saved by the Python host replays in Rust with the same screen and summary, and the reverse"],
            "PRD rust-host §6 criterion 3 · ADR-008",
        ),
    ),
    (
        M55,
        ["host", "rust"],
        "host(rs): tui with ratatui",
        body(
            "The TUI of #56, on ratatui + crossterm.",
            ["Follow/pause, filter, time, clear, palette; status line with lines, lost, missed, restarts, invalid, hidden, rate"],
            ["Same keys and status line as the Textual TUI"],
            "PRD rust-host RH-1 · ADR-008",
        ),
    ),
    (
        M55,
        ["host", "rust", "needs-hardware"],
        "host(rs): shell and dump",
        body(
            "The console on the host, and `esp-logcat dump`, which moves here from M5 and is born in Rust.",
            ["`shell`: one command (`--json`), interactive prompt with filtered logs, pipe (#30)", "`dump`: `#BEGIN/#END` block with CRC check (wire-format §3), needs #29 on the device"],
            ["`shell` matches #30", "`dump` of the C3's flash log passes the CRC check"],
            "PRD rust-host RH-1, RH-6 · #29 · #30",
        ),
    ),
    (
        M55,
        ["host", "rust", "release", "documentation"],
        "release: Rust host official, Python host retired (v0.5.0)",
        body(
            "Close M5.5.",
            [
                "`host-rs/` becomes `host/`; Python host removed",
                "Binaries per OS on the GitHub release; MANUAL install section without `.venv`",
                "`host-reviewer` for Rust",
            ],
            ["Every criterion in PRD rust-host §6 checked with evidence", "Tag `v0.5.0`"],
            "ADR-014 §6",
        ),
    ),
    # ---------------- M6 ----------------
    (
        M6,
        ["fw", "needs-hardware"],
        "fw: RtcLastWords",
        body(
            "Mini ring buffer in `RTC_NOINIT` memory with the last lines before a panic/WDT.",
            [
                "Magic number + CRC; validated on boot",
                "Previous boot's lines dumped as `I LASTWORDS: ...`, then buffer reset",
            ],
            ["Survives panic, WDT and software reset (not power loss)"],
            "PRD F8 · ARCHITECTURE §3",
        ),
    ),
    (
        M6,
        ["fw", "test", "needs-hardware"],
        "test: crash panic / wdt recovery",
        body(
            "Acceptance criterion 1.",
            ["`crash panic` and `crash wdt` from the console"],
            ["Next boot shows reason `PANIC`/`WDT` and the last lines before the crash"],
            "PRD §7 criterion 1",
        ),
    ),
    (
        M6,
        ["fw", "test", "needs-hardware"],
        "test: power-cut resilience",
        body(
            "Acceptance criterion 2: cut USB power during heavy logging, repeat N times.",
            ["Script on the host to log and correlate `seq` gaps"],
            [
                "Loss bounded by the last flush interval; ERROR lines never lost after flush; FS never corrupted"
            ],
            "PRD §7 criterion 2",
        ),
    ),
    (
        M6,
        ["documentation", "release", "needs-hardware"],
        "release: validate acceptance criteria and ship v1.0.0",
        body(
            "Close v1: every PRD §7 criterion validated with evidence.",
            ["CHANGELOG, README with GIF, tag `v1.0.0`"],
            ["Criteria 1–5 checked with links to evidence"],
            "PRD §7",
        ),
    ),
    # ---------------- M7 ----------------
    (
        M7,
        ["fw", "needs-hardware"],
        "fw: core dump to flash",
        body(
            "Store core dumps in the `coredump` partition and document analysis on the PC.",
            [],
            ["Core dump captured after `crash panic` and decoded on the PC"],
            "PRD §4, M7",
        ),
    ),
    # ---------------- Multi-board host ----------------
    (
        MB,
        ["host", "test", "needs-hardware"],
        "test: reference sessions from each ESP family (C3, C6, S3, ESP32)",
        body(
            "The host already reads boards that do not run esp32-logcat: the stock `blink` example on an ESP32-C6 shows up with levels, tags and colors (`invalid 0`). Record that evidence before the Rust host is written, so every multi-board feature has fixtures to test against. Recording uses the Python host's existing `--save`; no new Python feature.",
            [
                "For each board in the inventory (C3, C6, S3, ESP32 D0WD), a stock ESP-IDF example (`hello_world` or `blink`) flashed with `idf.py`",
                "Sessions saved over each port the board has (USB bridge and native USB-Serial-JTAG), each with a reset button press mid-session",
                "Files in `host/tests/fixtures/boards/<chip>-<port>.log`, with chip, bridge, VID:PID, IDF version and what was seen in a short README",
                "Board facts (bridge, VID:PID, banner, reset behaviour per port) added to `docs/context/hardware.md`",
            ],
            [
                "At least one session per family over each of its ports",
                "Known gaps listed in the README: the ESP32 banner (`ets ...`) not seen as a restart, glued ESP-IDF lines, other timestamp formats",
            ],
            "PRD v2 §6 criterion 3 · PRD rust-host RH-7 · #98",
        ),
    ),
    (
        MB,
        ["host", "rust"],
        "host(rs): ESP-IDF log lines from any firmware (timestamp formats, colors, glued lines)",
        body(
            "Read the native ESP-IDF log of any firmware as well as our own format: level, tag, timestamp and message, for the filters and the status line.",
            [
                "Timestamp variants: `(ms)`, `(hh:mm:ss.mmm)` and `(YYYY-MM-DD hh:mm:ss.mmm)` from `CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM`; ESP-IDF log v2 lines",
                "ANSI colors from `CONFIG_LOG_COLORS` stripped before parsing, never forwarded as markup",
                "Glued ESP-IDF lines split like our own (today `pipeline._LINE_START` knows only our format and `ESP-ROM:`)",
                "Unknown lines stay raw text (`invalid` only for lines that look like ours)",
            ],
            ["Every session from #103 replays with `invalid 0` and the right level for every ESP-IDF line"],
            "PRD v2 §6 criterion 3 · wire-format §1 · depends on #96",
        ),
    ),
    (
        MB,
        ["host", "rust"],
        "host(rs): chip and reset reason from the boot banner, on every ESP family",
        body(
            "Know which chip is on the other end and why it restarted, from what the ROM and the bootloader print, without our firmware.",
            [
                "ROM banners: `ESP-ROM:<chip>-<date>` (S2, S3, C-series, P4) and `ets <date>` (ESP32): both start a new boot (extends #98)",
                "`rst:0x.. (REASON)` in both spellings (`POWERON` and `POWERON_RESET`)",
                "Chip and revision from the banner and the bootloader (`chip revision: v0.x`), shown in the header next to the port",
                "With our firmware: the `info` command's chip and reset reason win over the guess",
            ],
            ["Each reference session shows its chip in the header and counts its reset"],
            "PRD rust-host RH-7 · #98 · `docs/context/hardware.md`",
        ),
    ),
    (
        MB,
        ["host", "rust", "needs-hardware"],
        "host(rs): ports and reset for every ESP USB bridge",
        body(
            "Find and open any ESP board without resetting it by accident, and reset it on request the right way for its wiring.",
            [
                "VID:PID table: Espressif USB-Serial-JTAG `303A:1001`, Espressif USB-OTG CDC, CH340 `1A86:7523`, CH9102/CH343 `1A86:55D4`/`55D3`, CP210x `10C4:EA60`, FTDI `0403:6001`/`6010`",
                "Open with DTR/RTS released on every bridge; reset per wiring: CH340K direct (RTS = EN), the classic two-transistor auto-reset circuit, native USB",
                "`esp-logcat ports` shows the kind of each port; with several candidates and no `--port`, list them (#97)",
                "Bridges with a serial number (CH9102, CH343) keep their identity when the COM number changes",
            ],
            ["Each board in the inventory is listed with the right kind, opened without a reset, and reset on request"],
            "`docs/context/hardware.md` · depends on #97",
        ),
    ),
    (
        MB,
        ["host", "rust", "needs-hardware"],
        "host(rs): several devices at once, identified by MAC",
        body(
            "A bench with several boards: one host session reads all of them, and a board keeps its identity when its COM number changes.",
            [
                "Identity by MAC: from the native USB serial number, from the bootloader/ROM output, or from our `info` command; the COM is only where it is now",
                "One reader thread per port, merged in the backend with the device on every line (ADR-014 §4)",
                "`stream`: a device column; filters per device; `--save` records the device",
                "`tui`: one pane per device or a merged view; status line per device",
            ],
            [
                "Three boards of different families read at once; unplugging and replugging one keeps its name and counters",
                "Session file format change, if any, goes through the spec first (wire-format §4)",
            ],
            "ROADMAP, after M5.5 · base for #85 · depends on #97",
        ),
    ),
    # ---------------- v2 · Multichip component ----------------
    (
        V2,
        ["fw", "ci"],
        "fw: build matrix for every v2 target (idf.py set-target)",
        body(
            "Build the component and the demo for every v2 target in the CI, before any board is brought up. Settles PRD v2 open question 2: the matrix is `idf.py set-target`, as the build is plain ESP-IDF since ADR-013.",
            ["CI matrix: esp32c3, esp32c6, esp32s3, esp32, esp32p4", "Per-target `sdkconfig.defaults.<target>` only where a target needs it"],
            ["The demo builds for every target in the CI"],
            "ADR-009 §2 · PRD v2 V2-2, §7 · depends on #91, #95",
        ),
    ),
    (
        V2,
        ["fw", "core"],
        "fw: SoC capabilities and Kconfig options instead of target checks",
        body(
            "Features follow what the SoC has, not its name, and the options a product changes live in Kconfig.",
            [
                "`SOC_*` capabilities (USB-Serial-JTAG, USB-OTG, cores, RTC memory) instead of `CONFIG_IDF_TARGET_*`",
                "Kconfig: writer task core, queue depth, partition label, sinks; read as `constexpr`",
            ],
            ["No `CONFIG_IDF_TARGET_*` left in the component", "Options documented in the component README"],
            "ADR-009 §3, §4 · PRD v2 V2-3, V2-4",
        ),
    ),
    (
        V2,
        ["fw"],
        "fw: UartSink and USB-OTG CDC sink",
        body(
            "One `Sink` per output peripheral, so chips without USB-Serial-JTAG (ESP32) and boards that use USB-OTG (S3) get the log and the console.",
            ["`UartSink` on UART0 (ESP32, and boards with a bridge only)", "USB-OTG CDC sink (S3, P4)", "Same wire format and console on every sink"],
            ["The ESP32 logs and answers the console over its CP210x/CH9102 bridge", "The S3 does the same over USB-OTG"],
            "ADR-009 §5 · PRD v2 V2-5",
        ),
    ),
    (
        V2,
        ["fw"],
        "fw: dual-core writer task and last words",
        body(
            "ESP32, S3 and P4 have two cores: the writer task and the last words must stay correct there.",
            [
                "Writer task affinity configurable (default `tskNO_AFFINITY`), created statically",
                "Last words with a single writer, no lock, no write during the panic",
                "Cycle measurement handles migration between cores (`esp_cpu_get_cycle_count()` is per core)",
            ],
            ["`crash panic` and `crash wdt` on each core of the ESP32 and S3 show the last lines on the next boot"],
            "ADR-009 §7 · PRD v2 V2-6 · depends on #31",
        ),
    ),
    (
        V2,
        ["fw", "test", "needs-hardware"],
        "fw: bring-up on ESP32-C6",
        body(
            "Run esp32-logcat on the ESP32-C6 DevKit (clone). Single core, RISC-V like the C3, with native atomics and USB-Serial-JTAG.",
            ["Flash size and partitions", "Logs on the native port and on the UART bridge", "Hook cost measured (native atomics, unlike the C3)"],
            ["PRD v1 §7 criteria 1-5 pass on the C6 (3 measured)", "Bring-up recorded in `docs/context/hardware.md`"],
            "PRD v2 V2-2, V2-7, §6 criterion 1 · bring-up issue template",
        ),
    ),
    (
        V2,
        ["fw", "test", "needs-hardware"],
        "fw: bring-up on ESP32-S3",
        body(
            "Run esp32-logcat on the ESP32-S3: dual core (Xtensa), USB-Serial-JTAG and USB-OTG.",
            ["Flash/PSRAM variant of the board", "Logs on USB-Serial-JTAG, USB-OTG CDC and the UART bridge", "Hook cost measured on each core"],
            ["PRD v1 §7 criteria 1-5 pass on the S3 (3 measured)", "Bring-up recorded in `docs/context/hardware.md`"],
            "PRD v2 V2-2, V2-7, §6 criterion 1 · needs #110 and #111",
        ),
    ),
    (
        V2,
        ["fw", "test", "needs-hardware"],
        "fw: bring-up on ESP32 (classic)",
        body(
            "Run esp32-logcat on the ESP32-D0WD-V3 boards (CH9102 bridge): dual core, no native USB, UART only.",
            ["UART0 as the only log and console port", "Reset behaviour of the CH9102 auto-reset circuit", "Hook cost measured on each core"],
            ["PRD v1 §7 criteria 1-5 pass on the ESP32 (3 measured)", "Bring-up recorded in `docs/context/hardware.md`"],
            "PRD v2 V2-2, V2-7, §6 criterion 1 · needs #110 and #111",
        ),
    ),
    (
        V2,
        ["fw", "test", "needs-hardware"],
        "fw: bring-up on ESP32-P4",
        body(
            "Run esp32-logcat on the ESP32-P4: dual core RISC-V, no radio. No board yet: this stays open until one is bought.",
            ["USB-Serial-JTAG, USB-OTG and UART", "Hook cost measured on each core"],
            ["PRD v1 §7 criteria 1-5 pass on the P4 (3 measured)", "Bring-up recorded in `docs/context/hardware.md`"],
            "PRD v2 V2-2, §5 (no radio)",
        ),
    ),
    (
        V2,
        ["fw", "release", "documentation"],
        "release: component on the IDF Component Registry (v2.0.0)",
        body(
            "Publish the component so any ESP-IDF project adds it with one line.",
            [
                "`idf_component.yml` metadata, examples, license, targets list",
                "Public API and Kconfig options under SemVer (PRD v2 §5)",
                "Docs in English for external users (PRD v2 §7, question 3)",
            ],
            [
                "A new ESP-IDF project adds the component from the Registry and sees the logcat without changing its code (PRD v2 §6 criterion 2)",
                "Every PRD v2 §6 criterion checked with evidence; tag `v2.0.0`",
            ],
            "ADR-009 §1, §6 · PRD v2 V2-1",
        ),
    ),
]


def gh(*args: str) -> str:
    r = subprocess.run([GH, *args], capture_output=True, text=True, encoding="utf-8")
    if r.returncode:
        raise SystemExit(f"gh {' '.join(args[:3])} failed: {r.stderr}")
    return r.stdout


def gh_json(*args: str) -> list[dict]:
    return json.loads(gh(*args))


def sync_labels() -> None:
    existing = {
        lbl["name"]
        for lbl in gh_json("label", "list", "-R", REPO, "--json", "name", "--limit", "200")
    }
    for name, desc, color in LABELS:
        if name not in existing:
            print(f"label     + {name}")
            if APPLY:
                gh("label", "create", name, "-R", REPO, "-d", desc, "-c", color)


def sync_milestones() -> dict[str, int]:
    existing = {
        m["title"]: m["number"]
        for m in gh_json("api", f"repos/{REPO}/milestones?state=all&per_page=100")
    }
    for title, desc, state in MILESTONES:
        if title not in existing:
            print(f"milestone + {title} [{state}]")
            if APPLY:
                created = json.loads(
                    gh(
                        "api",
                        f"repos/{REPO}/milestones",
                        "-f",
                        f"title={title}",
                        "-f",
                        f"description={desc}",
                        "-f",
                        f"state={state}",
                    )
                )
                existing[title] = created["number"]
    return existing


def sync_issues() -> None:
    existing = {
        i["title"]: (i["number"], {lbl["name"] for lbl in i["labels"]})
        for i in gh_json(
            "issue",
            "list",
            "-R",
            REPO,
            "--state",
            "all",
            "--limit",
            "500",
            "--json",
            "number,title,labels",
        )
    }
    for ms, labels, title, text in ISSUES:
        if title in existing:
            number, current = existing[title]
            missing = [lbl for lbl in labels if lbl not in current]
            if missing:
                print(f"issue #{number:<3} labels + {', '.join(missing)}")
                if APPLY:
                    gh("issue", "edit", str(number), "-R", REPO, "--add-label", ",".join(missing))
            continue
        print(f"issue     + [{ms.split(' ')[0]}] {title}  ({', '.join(labels)})")
        if APPLY:
            args = ["issue", "create", "-R", REPO, "-t", title, "-b", text, "-m", ms]
            for lbl in labels:
                args += ["-l", lbl]
            gh(*args)


def attach_foundation_prs(milestones: dict[str, int]) -> None:
    # `gh pr edit --milestone` only sees open milestones; M0 is closed, so use the REST API.
    number = milestones.get(MILESTONES[0][0])
    for pr in FOUNDATION_PRS:
        current = json.loads(gh("api", f"repos/{REPO}/issues/{pr}"))["milestone"]
        if number is None or (current and current["number"] == number):
            continue
        print(f"pr #{pr:<5} -> {MILESTONES[0][0]}")
        if APPLY:
            gh("api", "-X", "PATCH", f"repos/{REPO}/issues/{pr}", "-F", f"milestone={number}")


def main() -> None:
    sync_labels()
    milestones = sync_milestones()
    sync_issues()
    attach_foundation_prs(milestones)
    if not APPLY:
        print("\n(dry run — pass --apply to make these changes)")


if __name__ == "__main__":
    main()
