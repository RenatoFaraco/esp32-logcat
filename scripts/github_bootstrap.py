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
    ("host", "esp-logcat Python tool", "fbca04"),
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
]


def body(summary, scope, done, refs):
    lines = [summary, "", "### Scope"] + [f"- {s}" for s in scope]
    lines += ["", "### Done when"] + [f"- [ ] {d}" for d in done]
    lines += ["", f"**Refs:** {refs}"]
    return "\n".join(lines)


M1, M2, M3, M4, M5, M6, M7 = (m[0] for m in MILESTONES[1:])

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
                "Shared example lines in `host/tests/fixtures/` used by the C++ tests",
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
        ["host", "fw", "needs-hardware"],
        "host: esp-logcat dump",
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
