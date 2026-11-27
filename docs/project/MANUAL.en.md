# User manual — esp32-logcat

> For people who will **use** the system: flash the firmware, plug the board in and follow its
> logs on the PC with `esp-logcat`. How it works inside: `ARCHITECTURE.md` (Portuguese). Board
> facts and USB pitfalls: `hardware.md` (Portuguese).

> Versão em português: [MANUAL.md](MANUAL.md)

The terminal pictures in this manual are **real output from the board**: sessions recorded with
`scripts/record_session.py` and drawn by the same code as `esp-logcat stream`
(`scripts/manual_screenshots.py`, see [the end of this manual](#how-the-pictures-were-made)).

## Start here

With the firmware flashed and the board plugged in through the native USB-C port, open a
terminal (**PowerShell** or **Command Prompt**, `cmd`; in VS Code: *Terminal → New Terminal*)
and type:

**PowerShell**

```powershell
cd C:\dev\esp32-logcat
host\.venv\Scripts\Activate.ps1
esp-logcat ports
esp-logcat stream --reset
```

**Command Prompt (cmd)**

```bat
cd /d C:\dev\esp32-logcat
host\.venv\Scripts\activate.bat
esp-logcat ports
esp-logcat stream --reset
```

`Ctrl+C` stops it. The `esp-logcat` commands are the same in both terminals; only the
environment activation (2nd line) and a few pipe tricks (section 4) differ. First time on this
machine, no environment yet or no firmware on the board: section 1.

## Contents

1. [From zero to a running logcat](#1-from-zero-to-a-running-logcat)
2. [Reading the output](#2-reading-the-output)
3. [What happens in each situation](#3-what-happens-in-each-situation)
4. [Pipes, grep and files](#4-pipes-grep-and-files)
5. [The board's two ports](#5-the-boards-two-ports)
6. [Troubleshooting](#6-troubleshooting)
7. [Quick reference](#7-quick-reference)
8. [Console commands](#8-console-commands)
9. [The log in flash](#9-the-log-in-flash)

---

## 1. From zero to a running logcat

### 1.1 What you need

- An ESP32-C3 board with two USB-C ports (the one used here: `hardware.md`).
- Windows with ESP-IDF 6.1.1 (installed with EIM) and Python 3.12. The commands work in
  **PowerShell** and in the **Command Prompt (cmd)**, from the repository root
  (`C:\dev\esp32-logcat`); where the two differ, this manual shows both.
- **A cable on the USB-C of the native port (USB-Serial-JTAG)**, plugged **straight into the
  PC**, not into a hub: behind a hub, a reset through the EN pin sometimes makes Windows lose
  the port (`hardware.md`, item 5). The other USB-C (CH340K bridge) is optional: it carries the
  full console.

### 1.2 Install the PC tool (once)

```powershell
cd host
py -3.12 -m venv .venv
.venv\Scripts\python -m pip install -e ".[dev]"
cd ..
```

(The same commands work in cmd; replace `cd ..` with `cd /d C:\dev\esp32-logcat`.)

This creates the command `host\.venv\Scripts\esp-logcat.exe`. To avoid typing the path,
**activate the environment** in each terminal you open and use just `esp-logcat`, as the rest
of this manual does:

| Terminal | Activate | Sign that it worked |
|----------|----------|---------------------|
| PowerShell | `host\.venv\Scripts\Activate.ps1` | the prompt starts with `(.venv)` |
| cmd | `host\.venv\Scripts\activate.bat` | same |

Without activating, use the full path: `host\.venv\Scripts\esp-logcat.exe stream`.

If PowerShell says that "running scripts is disabled on this system", allow local scripts for
your user (once) with `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`, or use cmd, or the
full path.

### 1.3 Flash the firmware

With the board plugged in through the native port:

```powershell
cd firmware\examples\demo   # in the ESP-IDF PowerShell (EIM's shortcut)
idf.py -p COM6 flash
```

Replace `COM6` with your machine's native port (next step). The first build takes a few
minutes; later ones, seconds.

### 1.4 Find the ports

```powershell
esp-logcat ports
```

![esp-logcat ports](manual/img/ports.svg)

The port marked **`native USB-Serial-JTAG`** is the logcat one. The COM number changes from PC
to PC (and sometimes from USB socket to USB socket): the tool never hard-codes a number.

### 1.5 Watch the logs

```powershell
esp-logcat stream --reset
```

Without `--port`, the tool picks the native port by itself when there is exactly one.
`--reset` restarts the board when connecting, so you see the boot from the start:

![esp-logcat stream --reset](manual/img/boot.svg)

What shows up, top to bottom:

1. `-- connected to COM6 --` — a notice from the tool (it goes to stderr, section 2.3).
2. **ROM messages** (`ESP-ROM:`, `rst:0x15 ...`) — raw text, dimmed.
3. **ESP-IDF boot** (`I (24) boot: ...`) — ESP-IDF's native format, in the level's color:
   these are the logs printed before the firmware installs the logcat hook.
4. **Logcat lines** (`0000:000000 0000000027 I APP: ...`) — from here on, every `ESP_LOGx` of
   the application and of ESP-IDF itself goes through the esp32-logcat core.
5. The demo app's **burst** (`BURST`) and the **heartbeat**, once a second.
6. `Ctrl+C` stops and prints a **summary** of the session.

That's it: the logcat is running.

---

## 2. Reading the output

### 2.1 Anatomy of a line

```
0000:000107 0000001030 I HEARTBEAT: tick 1, capture 6989 cycles (max 181321), stack free 2780 B
│    │      │          │ │          └─ message (up to 127 characters; a cut one ends in "...")
│    │      │          │ └─ tag (who logged; up to 15 characters)
│    │      │          └─ level: E error · W warning · I info · D debug · V verbose
│    │      └─ uptime: milliseconds since boot (10 digits)
│    └─ seq: line number in this boot (6 digits, wraps to 0 after 999999)
└─ boot: boot counter, kept on the board (1 to 9999, then 1 again; 0000 = unknown)
```

`seq` is the key piece: **every line the firmware accepted gets a number**, in order. If a
number is missing, a line was lost, and the tool says so (section 3.2). Lines silenced by the
tag filter do **not** get a number: they never look like losses.

Each line is at most 170 bytes of printable ASCII (full format: `docs/context/spec/wire-format.md` §1).

### 2.2 Colors

| Level | Color | Example |
|-------|-------|---------|
| `E` error | red | `E APP: an error, letter E` |
| `W` warning | yellow | `W APP: a warning, letter W` |
| `I` info | green | `I HEARTBEAT: tick 1 ...` |
| `D` debug | bright blue | — |
| `V` verbose | the terminal's default color | — |

- On logcat lines the header (`boot:seq uptime`) is dimmed; level and tag are bold.
- ESP-IDF lines (`I (24) boot: ...`) use the level's color, without bold.
- Raw text (ROM, `printf`) is dimmed.
- Invalid lines get a red label: `!! torn: ...` (section 3.6).
- Control characters coming from the board are shown escaped (`\x1b`, `\u202E`): the board can
  never drive your terminal.

Outside a terminal (in a pipe or a file) the colors go away by themselves.

### 2.3 Notices and summary (stderr)

Everything that is **not** a line from the board goes to **stderr**, between `-- --`:

| Notice | Meaning |
|--------|---------|
| `connected to COM6` | port open (with `(was COM5)` if the board came back under another number) |
| `waiting for COM6: not available` | the port does not exist (board off, cable out): retrying every 0.5 s |
| `waiting for COM6: port busy` | another program has the port open |
| `COM6: connection lost (...)` | the port dropped while reading; the tool waits and reconnects |
| `N lines lost before seq X` | N `seq` numbers are missing: lines lost |
| `N lines written before the connection not received` | lines written before you connected (not a loss on the board) |
| `seq X arrived after Y` | a line out of order (normal in bursts from several tasks) |
| `device restarted (...)` | the board restarted (`seq` and uptime went back to zero) |
| `uptime went back ...` / `seq went back ...` | probably a corrupted line |

On exit (`Ctrl+C`) the summary counts the lines of each kind and the session's events:

```
100 lines, 44 log, 46 esp-idf, 10 raw, 0 invalid | lost 72, missed before connecting 0, reordered 0,
restarts 0, uptime back 0, seq back 0, reconnects 0, overflows 0
```

### 2.4 Lines the firmware writes itself (`LOGCAT`)

The firmware explains its own losses with `W LOGCAT:` lines, which carry a `seq` like any
other:

| Line | When |
|------|------|
| `W LOGCAT: dropped N lines: log queue full` | the code logged faster than the writer task could write: the queue (32 lines) filled up and N lines were dropped **without blocking whoever logged** |
| `W LOGCAT: N lines not sent to usb: no reader` | the native port stayed closed (nobody reading) and N lines could not be sent through it |

---

## 3. What happens in each situation

### 3.1 Boot with `--reset`

Seen in section 1.5. Without `--reset`, the tool only shows what arrives after you connect (the
board keeps running).

### 3.2 Burst: the queue filled up

At boot the demo app logs 100 lines in a row, faster than the writer task can write them (the
UART mirror takes ~15 ms per line). The 32-line queue fills up and the rest is **dropped and
counted** — the code that logs never waits:

![burst with a full queue](manual/img/burst.svg)

- `-- 42 lines lost before seq 000074 --` and `-- 30 lines lost ... --`: the tool saw the
  holes in `seq` (27 → 74 and 75 → 106).
- `W LOGCAT: dropped 72 lines: log queue full`: the firmware confirms it: 42 + 30 = 72.

This is the intended behavior: **losing lines visibly is better than delaying the
application** (ADR-004).

### 3.3 Port closed for a while, then reopened

The board keeps logging with nobody reading the native port, and it does **not** stall because
of that (each log still takes microseconds). On reopening:

![port reopened](manual/img/reopen.svg)

`W LOGCAT: 39 lines not sent to usb: no reader` says how many lines the native port could not
send while it was closed. The tool reports no loss: the first line is already the current one,
and `seq` continues from it.

### 3.4 Port busy

If another program (an `idf.py monitor`, VS Code's serial monitor, another esp-logcat)
holds the port, the tool waits and connects as soon as the port is free:

![port busy](manual/img/busy.svg)

### 3.5 Cable unplugged, board restarted, COM number changed

The tool **never gives up**: if the port drops, it says so, waits and reconnects.

```
-- COM6: connection lost (ClearCommError failed ...) --
-- waiting for COM6: not available --
-- connected to COM7 (was COM6) --
```

If the board comes back under **another COM number**, it is found again by its serial number
(on the native port, the serial number is the board's MAC address) or, on the CH340K bridge,
by its physical USB position. If the board restarts, `-- device restarted (...) --` shows up.

### 3.6 Torn line (`!! torn`)

A line without its final `\n` — typically one cut in the middle by a reset or by closing the
port — shows up as `!! torn: <bytes>`. It is displayed, never mistaken for a valid line, and
counted in the summary (`invalid (torn N)`).

### 3.7 PC time alongside (`--time`)

```powershell
esp-logcat stream --time
```

![esp-logcat stream --time](manual/img/time.svg)

The first column is the PC time at which the line arrived (`HH:MM:SS.mmm`), handy to match
other records. The board's uptime stays in the line.

### 3.8 Filters: only what matters

Filters follow `adb logcat` syntax: each argument is `TAG:LEVEL`, and a line shows up when its
level is **as severe or more severe** than its tag's.

```powershell
esp-logcat stream "*:W"                         # warnings and errors only, every tag
esp-logcat stream "*:S" "PRESSURE:D"            # only the PRESSURE tag, down to debug
esp-logcat stream "*:W" "HEARTBEAT:I"           # warnings and errors, plus the heartbeat
esp-logcat stream --reset "*:S" "HEARTBEAT:I" "LOGCAT:W"
```

![esp-logcat stream with a filter](manual/img/filter.svg)

| Part | Meaning |
|------|---------|
| `TAG:L` | tag `TAG` at level `L` or more severe. Levels, most severe first: `E` `W` `I` `D` `V` |
| `TAG:S` or `TAG:N` | silences the tag (`S` is adb's "silent", `N` the firmware's `None`) |
| `TAG` | same as `TAG:V` (everything from that tag) |
| `*:L` | the level of **every other tag**. Without `*:`, other tags stay visible |

- A tag's rule beats `*`, in any order. The same tag twice: the last one wins.
- Tags are case-sensitive (`HEARTBEAT` ≠ `heartbeat`); levels are not.
- Lines without a tag (ROM text, `!! torn`) **always show up**.
- **Filters only hide:** `seq` gaps and the summary still count every line, and the summary
  says how many were hidden (`N hidden by filter`). Hiding a tag never turns into "lines lost".
  stderr starts with `-- filter: ... --` as a reminder of what is active.
- `seq` notices (`lines lost`, `arrived after`) refer to **every** line, hidden ones included:
  they may name a `seq` you are not seeing. When silencing everything with `*:S`, keep
  `"LOGCAT:W"`, or you miss the lines in which the board itself explains its losses.
- Several filters may share one argument (`"*:S HEARTBEAT:I"`), and filters and options may
  come in any order (`stream "*:S" --time "APP:D"`).
- An invalid filter (`APP:F`, `APP:X`) exits with an error message explaining what is allowed.

**Quotes:** in PowerShell and cmd, `"*:W"` works with or without quotes. In bash (Git Bash,
Linux), quote it, or `*` turns into file names.

### 3.9 Replaying a saved log (`replay`)

`replay` reads a saved file and runs it **through the same path as `stream`**: same parser,
same colors, same filters, the gap and restart notices and the summary. Use it to study a
session later, without the board.

```powershell
esp-logcat replay session.log                         # everything, in color
esp-logcat replay session.log "*:W"                   # warnings and errors only
esp-logcat replay session.log "*:S" "HEARTBEAT:I" > heartbeat-only.log
```

![esp-logcat replay with a filter](manual/img/replay.svg)

The picture is the `replay` of a real capture with **two boots** (the fixture
`host/tests/fixtures/two_boots_esp32c3.log`): the `device restarted` notice separates them, and
the summary counts the losses of both bursts (`lost 144`) and the restart (`restarts 1`).

- The file may come from `esp-logcat stream > file` (in PowerShell 5.1 that writes UTF-16, and
  `replay` reads it anyway), from `cmd /c "... > file"`, from a serial capture or, once it
  exists, from the flash `dump` (M5).
- `-` instead of a file reads stdin: `type session.log | esp-logcat replay - "*:W"`.
- The file is read in one go, without the original pace. A file saved with `stream --time`
  is read too: the time column is dropped (showing it again comes with #21).
- `--lenient` accepts hand-edited lines: an empty message that lost its space (`TAG:` at the
  end of the line) and a last line without a line break. Without it, `replay` is as strict
  as the board.
- A read error in the middle of the file (drive removed, network share dropped) shows up
  as `-- file: read error (...) --` and the command exits with status 1: never a partial
  replay that looks complete.

**What may differ from what you saw live** (in a plain log; a session saved with `--save`, section 3.10, comes back identical):

- A file saved **with a filter** lacks the hidden lines: `replay` shows `seq` gaps where
  they were (`N lines lost`). To study a session later, save it unfiltered and filter in
  `replay`.
- Invalid lines saved as `!! torn: ...` come back as raw text, not as invalid lines.
- The file does not know when the port was reopened: a `seq` jump that was "not received
  before the connection" live shows up as `lines lost`.

### 3.10 Saving the session (`--save`)

`--save FILE` writes **the whole session**: every line the board sent (even the ones the
filter hides from the screen), with the **PC date and time** next to each, and the connection
notices. Replaying that file shows **exactly** what you saw live: the same lines, the same
notices and the same summary.

```powershell
esp-logcat stream --save session.log "*:W"     # the screen shows W/E only; the file keeps all
esp-logcat replay session.log --time           # the saved PC time comes back on screen
esp-logcat replay session.log "*:S" "APP:D"    # filter later, any way you like
```

The file is text: it opens in an editor and works with `findstr`/`Select-String`:

```
# esp-logcat session v1
# 2026-11-08T22:38:38.743 notice connected connected to COM6
2026-11-08T22:38:38.945 ESP-ROM:esp32c3-api1-20210207
2026-11-08T22:38:39.011 0000:000001 0000000027 W APP: a warning, letter W
```

- Each board line: the PC date and time (with milliseconds) + the exact bytes that arrived.
- `#` lines belong to the file: the header, connection notices (`notice`) and lines a plain record
  could not keep intact (`raw`, in hex): no line break, like one cut by a disconnection, or a
  `\r`. In a plain log (no header), `#` is device text.
- It is written line by line: Ctrl+C or a PC crash does not lose what already arrived.
- `--save` overwrites the file. It also works with `replay` (to convert a plain log).

---

### 3.11 Interactive interface (`tui`)

To follow the board with more control than `stream`: scroll the history, pause, change the
filter without restarting and see the counters all the time.

```powershell
esp-logcat tui                                  # the native port, if there is exactly one
esp-logcat tui --reset "*:W" "HEARTBEAT:I"      # the same filters as stream
esp-logcat tui --replay session.log             # no board: a saved log
esp-logcat tui --save session.log               # save the session (section 3.10) while showing
```

![esp-logcat tui](manual/img/tui.svg)

| Key | What it does |
|-----|--------------|
| `space` | pause the screen / follow again. While paused, lines keep arriving (counters and history); resuming shows them all |
| `f` | edit the filter (`*:W HEARTBEAT:I`); `Enter` applies it and **redraws the history**, `Esc` cancels. An invalid filter: a warning, and the previous one stays |
| `t` | show or hide the PC time on each line |
| `c` | clear the screen (the counters stay) |
| `q` or `Ctrl+C` | quit; the session summary is printed in the terminal |

- The header shows the port and its state (`connected`, `waiting`, `disconnected`).
- The status line shows lines, lost, `missed`, restarts, invalid, hidden by the filter and the
  rate (lines/s).
- Scroll with the mouse or `PgUp`/`PgDn`; the history keeps the last 10 thousand lines.
- Works in Windows Terminal and the VS Code terminal. `stream` is still the one for pipes and
  files; `tui` is for watching.

---

## 4. Pipes, grep and files

stdout gets **only the board's lines, exactly as the board wrote them** — no colors, no
notices. Notices and the summary go to stderr. This allows:

**PowerShell**

```powershell
# filter live
esp-logcat stream | Select-String "HEARTBEAT"
esp-logcat stream | Select-String -CaseSensitive " W ", " E "    # warnings and errors only

# first 20 lines only (the tool stops by itself)
esp-logcat stream | Select-Object -First 20

# save the session with the board's exact bytes (and the notices in another file)
cmd /c "esp-logcat stream > session.log 2> notices.log"
```

> **Careful in Windows PowerShell 5.1:** `esp-logcat stream > session.log` writes the file as
> **UTF-16**, not with the board's bytes. Use the `cmd /c` above (or PowerShell 7, whose `>`
> writes UTF-8). In a pipe, PowerShell reports "exit code 255" when it stops the process at the
> end of the pipeline (`Select-Object -First`): that is PowerShell stopping `stream`, not an
> error from it.

**Command Prompt (cmd)**

```bat
:: save the session (the board's exact bytes) and the notices in another file
esp-logcat stream > session.log 2> notices.log

:: then filter the saved file
findstr HEARTBEAT session.log
findstr /C:" W " /C:" E " session.log
```

> In cmd, `findstr` **cannot filter live**: in a pipe (`stream | findstr ...`) it buffers its
> output and shows nothing until the end (tested). To filter live, use PowerShell
> (`Select-String`) or the tool's own filters (section 3.8), which work the same in both
> terminals. cmd's `>` writes the exact bytes.

The tool's filters combine with all of this: `esp-logcat stream "*:W" > warnings-and-errors.log`.

---

## 5. The board's two ports

| | **Native (COM6)** — the logcat | **UART bridge (COM10)** — the console |
|---|---|---|
| ROM, bootloader, early boot (ESP-IDF format) | yes | yes |
| Logcat lines | yes, with `\n` | yes (mirror), with `\r\n` |
| `printf` and text that is not a log | no | yes |
| `W LOGCAT` notices | yes | yes |
| Does opening the port reset the board? | no | no, if opened with DTR/RTS released (esp-logcat does that; `idf.py monitor` only with `--no-reset`) |

Use the **native** port for the logcat. Use **COM10** when you need the full console
(`esp-logcat stream --port COM10` works too) or while flashing through the native port.

---

## 6. Troubleshooting

| Symptom | Likely cause | What to do |
|---------|--------------|------------|
| `cannot choose a port (found: ...); use --port` | no native port, or more than one | run `esp-logcat ports` and pass `--port COMx` |
| `waiting for COM6: port busy` | another program has the port open | close the serial monitor (VS Code, `idf.py monitor`, another terminal) |
| `waiting for COM6: not available` forever | board off, a charge-only cable, or the port changed number before the board was seen in this session | check the cable; run `esp-logcat ports` |
| `esp-logcat` is not recognized | the environment is not active in this terminal | activate it (section 1.2) or use `host\.venv\Scripts\esp-logcat.exe` |
| The native port vanished after a reset through the CH340K; Windows shows "Unknown USB Device" | the board is behind a hub (`hardware.md`, item 5) | unplug and replug the cable; plug the native port straight into the PC |
| Many `lines lost` under heavy traffic | the application logs more than ~70 lines/s (the UART mirror's limit) | expected: losses are counted and never stall the application. The 1 kHz app (#13) will turn the mirror off |
| `W LOGCAT: N lines not sent to usb` | the native port stayed closed | normal: those are the lines from while nobody was reading |
| Nothing on COM6, but COM10 shows the logs | old firmware, from before the logcat | flash the current firmware (section 1.3) |
| Flashing fails with the port open | `stream` is holding the COM port | stop `stream` (`Ctrl+C`) before flashing, or flash through the other port |

---

## 7. Quick reference

```
esp-logcat ports                         list the ports and mark the native one
esp-logcat stream [options] [TAG:L ...]  live logs, with optional filters (section 3.8)
    "*:W"  "*:S" "TAG:D"  ...            logcat filters: levels E W I D V, S/N silences
    --port COMx                          port (default: the native one, if there is exactly one)
    --baud N                             baud rate (default 115200; only matters on COM10)
    --reset                              restart the board when connecting
    --time                               show the PC time on each line
    --save FILE                          save the whole session (section 3.10)
esp-logcat replay FILE [TAG:L ...]      replay a saved log, with filters (section 3.9)
    -                                    instead of FILE: read stdin
    --lenient                            accept hand-edited lines
    --time                               show the PC time saved in a session
    --save FILE                          save it again as a session
esp-logcat tui [options] [TAG:L ...]    interactive interface (section 3.11):
    --port, --reset, --save, --replay FILE, --lenient
esp-logcat shell [COMMAND | TAG:L ...]  board commands (section 8): one, or the prompt
    --json                               one command: the reply as JSON
    --port, --baud
esp-logcat --version
```

Not implemented yet: `dump` (milestone M5). The commands the board understands are in
[section 8](#8-console-commands).

---

## 8. Console commands

The board takes commands on the **native port** (the same one as `stream`), and
`esp-logcat shell` is what talks to it. The exact contract is in
`docs/context/spec/wire-format.md` sections 2 and 3.

### 8.1 Sending a command

**One command**: the reply is printed and the program ends. The exit code is 0 when it worked
and 1 when it did not, handy for scripts:

```powershell
esp-logcat shell info
esp-logcat shell log level APP D
esp-logcat shell stats --json
```

![esp-logcat shell with one command](manual/img/shell.svg)

**Interactive**: without a command, a prompt opens. Log lines keep appearing above what you type,
with the same filters as `stream` (section 3.8):

```
esp-logcat shell "*:W"
logcat> stats
logcat> log level HEARTBEAT D
logcat> quit
```

Here only `HEARTBEAT` shows; its `D` lines appear after `log level HEARTBEAT D` and go away
after `I`:

![esp-logcat shell interactive](manual/img/shell_prompt.svg)

At the prompt, `help` lists the commands, the arrow keys bring back earlier ones, and Ctrl+D (or
`quit`) leaves.

**Several commands from a script**: one per line, on standard input.

```powershell
"info`nlog level APP D`nstats" | esp-logcat shell "*:E"
```

The shell numbers the requests itself: you type `info`, it sends `5312 info` and waits for the
reply with that number. It waits 2 s for a reply; `reboot` and `crash` wait up to 10 s for the
restart. Windows allows one program per port at a time: close `stream` or `tui` first.

### 8.2 What goes over the wire

The shell shows the reply in a readable form. Underneath, the board sends:

| Line | What it is |
|------|------------|
| `#> <id> key=value ...` | data; there may be more than one |
| `#. <id> ok` | end, success |
| `#! <id> <code> <text>` | end, failure |
| `#BEGIN <id> ram <bytes>` … `#END <id> <crc32>` | a block holding whole log lines |

Every reply ends in exactly one `#.` or `#!` (except `crash`, which does not reply). `--json`
builds an object from those fields; numbers become numbers.

### 8.3 Each command, with real output

The output below was captured on the board with `esp-logcat shell` itself.

**`info`**: who the board is.

```
> esp-logcat shell info
fw         v0.3.0-23-g92b757f
proto      1
chip       esp32c3
idf        6.1.0
uptime_ms  26670
boot       5
reset      USB
flash      mounted
```

| Field | Meaning |
|-------|---------|
| `fw` | firmware version (`git describe` at build time) |
| `proto` | console protocol version |
| `chip` | the chip |
| `idf` | ESP-IDF version |
| `uptime_ms` | time since boot, in ms |
| `reset` | why it restarted: `POWERON` (power-up), `USB` (reset over USB), `EXT` (EN pin), `SW` (`reboot`), `PANIC` (crash), `INT_WDT`, `TASK_WDT` or `WDT` (watchdogs), `BROWNOUT` (low voltage), `DEEPSLEEP`, `SDIO`, `JTAG` |
| `flash` | the log in flash: `mounted`, `formatted` or `failed` (section 9) |

A field the board cannot measure is left out: it never comes as `0`. `boot` is the same number
that starts the lines; it is left out if the board could not read the counter.

**`stats`**: the logging engine's counters. Read-only: it clears nothing.

```
> esp-logcat shell stats
dropped             74
queue               0/32
queue_max           32
capture_cycles      22264/43685
writer_stack_free   2688
console_stack_free  2664
not_sent.usb        23
not_sent.console    2
not_sent.ram        0
not_sent.flash      0
tail_skipped        0
reply_failed        0
heap_min            291472
flash.files         1
flash.bytes         28392
flash.limit         458752
flash.unsynced      447
flash.rotations     0
flash.removed       0
flash.repaired      0
flash.failed        0
flash.sync_failed   0
flash.lost          0
fs.used             65536
fs.total            524288
```

As JSON (`esp-logcat shell stats --json`):

![esp-logcat shell stats --json](manual/img/shell_json.svg)


| Field | Meaning |
|-------|---------|
| `dropped` | lines refused because the queue was full, since boot |
| `queue` | lines in the queue now / capacity |
| `queue_max` | the most the queue has held since boot |
| `capture_cycles` | cost of logging to whoever logs, in CPU cycles: last / maximum |
| `writer_stack_free`, `console_stack_free` | smallest stack headroom seen in each task, in bytes |
| `not_sent.<output>` | lines that output refused since boot (`usb`, `console` = UART mirror, `ram`, `flash`) |
| `tail_skipped` | lines left out of `log tail` because a block was being sent |
| `reply_failed` | replies or reply lines that could not go out |
| `heap_min` | the lowest free heap seen since boot, in bytes |
| `flash.*`, `fs.*` | the log kept in flash: see section 9.4 |

Counters only go up; for a rate, read twice and subtract.

**`log level <tag> <level>`**: changes a tag's level while the board runs. Levels: `E`, `W`, `I`,
`D`, `V`, or `N` to silence it. The tag `*` changes the default level.

```
> esp-logcat shell log level HEARTBEAT D
ok
```

After that, the `D` lines of that tag show up in `stream`:

```
0000:000120 0000011030 D HEARTBEAT: debug detail of tick 11: shown after `log level HEARTBEAT D`
```

The board keeps up to 16 tags. The 17th is an error, and those already in the table can still
change:

```
> esp-logcat shell log level TAG14 W
error: full: tag table full (16 tags)
```

The level does not survive a restart. The tag is compared with the name used in the code, not
with what the line shows: in the log, a space or `:` in a tag appears as `_`.

**`log tail`**: the last 32 lines kept in RAM, as a block.

![esp-logcat shell log tail](manual/img/shell_tail.svg)


The shell reads the block by the size announced in `#BEGIN` and checks the CRC-32 before
showing the lines. While the block goes out, the live log stays off the USB. After a block under load you may see
`W LOGCAT: N lines not sent to usb: no reader`: those are the lines of that interval.

**`log sync`**: makes everything logged so far durable in flash. It answers `ok` only after the
sync, and an error if it could not: `failed` (the sync failed, or the board logged nonstop and
its queue did not empty within 1.2 s) or `unsupported` (no flash in this run).

```
> esp-logcat shell log sync
ok
```

**`log dump`**: the logs kept in flash, one block per file, oldest first. The board syncs the
flash first, and each file comes with the size it had at that moment: what is logged during the
dump waits for the next one. The shell checks each block's CRC and closes each one with a
summary line:

```
> esp-logcat shell log dump
0082:000119 0000008808 W LOGCAT: log cleared by console: 1 files, 3239 bytes removed
...
log_030.txt: 1388 lines, 65439 bytes, CRC ok
...
log_036.txt: 49 lines, 5636 bytes, CRC ok
```

With the flash full (448 KB in 7 files), the dump takes about 1 s over the native USB. While it
goes out, the live log does not go to the USB (as with `log tail`), and a request made meanwhile
is answered only at the end. If the rotation removes a file during the dump (very heavy
logging), the board replies `failed log rotated during dump`, and the blocks that already
arrived whole are valid. The Python shell still shows that case as `dump block damaged` (the
live log that comes back afterwards goes into the block's byte count); the Rust host will show
the reason (#101). Saving the files on the PC is `esp-logcat dump`, which comes with the Rust host
(#101).

**`log clear`**: removes every log file from flash and starts again in an empty `log_000.txt`.
**It cannot be undone.** The first line of the new log records how many files and bytes went,
and its boot and uptime say when:

```
> esp-logcat shell log clear
ok
```

Error replies: `busy` (a `log dump` is going out; nothing was removed), `failed` (a file could
not be removed, or the board kept logging and the request was not served in time; files may be
left, and another `log clear` finishes the job) or `unsupported` (no flash in this run).

**`reboot`**: replies and restarts; the shell waits to see the restart. The next `info`
shows `reset SW`.

```
> esp-logcat shell reboot
ok: the device restarted
```

**`crash panic`** and **`crash wdt`**: bring the board down on purpose, to test recovery. They do
not reply: the shell counts the restart as success. Afterwards, `info` shows `reset PANIC` or
`reset INT_WDT`:

```
> esp-logcat shell crash panic
ok: the device restarted
> esp-logcat shell info
fw         v0.3.0-23-g92b757f
proto      1
chip       esp32c3
idf        6.1.0
uptime_ms  175
boot       6
reset      PANIC
flash      mounted
```

### 8.4 Errors

Every error goes to stderr, and the exit code is 1.

| What you see | When |
|--------------|------|
| `error: bad_args: wrong arguments` | missing, extra or invalid argument (`log level APP x`) |
| `error: full: tag table full (16 tags)` | the tag table is full |
| `error: unsupported: no flash storage` | the flash did not mount in this run (`log sync`, `log dump`, `log clear`) |
| `error: busy: a log dump is being sent: try again after it` | `log clear` during a `log dump` |
| `error: failed: ...` | `log sync`, `log dump` or `log clear` could not do it (the text says what) |
| `error: timeout: host not reading` | the board could not send the reply in time |
| `error: no reply in 2 s` | no reply at all: a hung board, or firmware without a console |
| `error: no reply: the device restarted` | the board restarted during the request (or `the port closed`) |
| `error: no restart seen in 10 s` | `reboot` or `crash` without a restart |
| `error: dump block damaged: ...` | a `log tail` or `log dump` block arrived incomplete or with a wrong CRC |
| `unknown command: ...` | the word is not a board command; the shell does not send it |

The codes `bad_args`, `full`, `busy`, `failed`, `unsupported` and `timeout` come from the board; the others from
the shell. A board that gets a command it does not know answers `bad_command`, but the shell
stops that earlier. Commands are lowercase with no abbreviations; levels are uppercase.

---

## 9. The log in flash

The board keeps everything it logs in a 512 KB partition of its own flash (LittleFS). The log
survives a `reboot`, a crash and a power cut; it shows on the PC through the USB (as always) and,
once `dump` arrives (#29), also downloaded from flash.

### 9.1 Where and how much

- Files `log_000.txt`, `log_001.txt`… of up to 64 KB each, at most 448 KB together. When full,
  the oldest is removed: the most recent log always stays.
- Each line is the same one `stream` shows. When it reopens a file that ended inside a line (a
  power cut at the wrong moment), the board closes the piece with a special byte, and it reads
  as `!! torn`, never glued to the next line.

### 9.2 The boot line

At the start of every boot, the board says which boot this is and what it found in flash:

```
0006:000000 0000000076 I LOGCAT: boot=6 reason=SW fw=v0.3.0-23-g92b757f
0006:000001 0000000084 I LOGCAT: flash: mounted, 6 files, 390780 bytes of 458752, last line whole, littlefs 412/512 KB
```

In the first line:

| Part | Meaning |
|------|---------|
| `boot=6` | the boot counter, kept in NVS: it goes up by 1 at every boot, whatever the reason. It is the same number that starts every line (`0006:`), which tells one boot's log from another's in flash |
| `reason=SW` | why it restarted: the same names as `info` (section 8.3); `UNKNOWN` if the chip did not say |
| `fw=...` | the firmware version running |

If NVS was erased (full, or from another ESP-IDF version), `W LOGCAT: boot counter restarted:
NVS was erased` comes first, and the count starts again at 1. If the board could not read the
counter, `E LOGCAT: boot counter unavailable: lines carry boot 0000` comes instead.

In the second:

| Part | Meaning |
|------|---------|
| `mounted` | the partition was found with the previous log. `formatted (first use)`: the partition was blank and was formatted. If it fails to mount for another reason, `W LOGCAT: flash: FORMATTED: ... the previous logs are lost` comes first. `mount failed`: the log is not kept in this run |
| `6 files, 390780 bytes of 458752` | how much log is kept, and the limit |
| `last line whole` | the last file ended on a whole line. `last line torn: closed` means it ended inside a line, and the board closed it |
| `littlefs 412/512 KB` | how much of the partition LittleFS itself uses (blocks and metadata) |

To see this line, open the stream with `--reset`: without it, the line is gone by the time the
port opens.

### 9.3 What is lost, and when

Writing to the file is not enough: only a **sync** makes the log durable. The board syncs:

- **every 5 s**, if there is something new (even under heavy logging: it is checked at every
  line);
- **after an ERROR line**, as soon as the burst of lines ends: at once on a quiet board, within
  ~2 s under load. Never more than **one sync a second**: an error storm does not wear the
  flash out;
- **when you ask** (`esp-logcat shell log sync`, section 8.3) and before a `reboot`.

Why the ERROR does not sync inside its own line: a sync erases a flash sector (tens of ms), and
meanwhile the 32-line queue fills up. The lines dropped would be exactly the ones after the
error, which explain it.

| Situation | What stays in flash |
|-----------|---------------------|
| `esp-logcat shell reboot` | everything up to the reboot |
| `esp-logcat shell log sync` answered `ok` | everything logged before the request |
| `crash panic`, `crash wdt`, a hang | everything up to the last sync: up to 5 s before, an ERROR line up to ~1 s (`crash` mimics a real crash and writes nothing first) |
| Power cut | the same: up to 5 s, ERROR up to ~1 s |
| Full partition | does not happen: the oldest file is removed first |

In every case, the lines still in the queue (up to 32) are lost too.

### 9.4 Following it with `stats`

`esp-logcat shell stats` (section 8.3) shows the flash in the `flash.*` and `fs.*` fields:

| Field | Meaning |
|-------|---------|
| `flash.files`, `flash.bytes`, `flash.limit` | files, bytes kept, and the limit |
| `flash.unsynced` | bytes written but not yet in flash (what a cut now would lose) |
| `flash.rotations`, `flash.removed` | new files started, and old ones removed to make room |
| `flash.repaired` | cut lines the board closed when it reopened a file |
| `flash.failed`, `flash.sync_failed`, `flash.lost` | lines that did not go in, writes that failed, bytes lost to that. Expected: 0 |
| `fs.used`, `fs.total` | what LittleFS uses of the partition, and its size |
| `flash.sync_period`, `flash.sync_error`, `flash.sync_request` | syncs made since boot, by reason: the 5 s period, an ERROR line, a request (`log sync`, `reboot`) |
| `erase.sectors` | 4 KB sectors erased in the whole flash since boot (the log and NVS): what wears the flash. Divided by the uptime, it is the wear rate |

`info` gained `flash`: `mounted`, `formatted` or `failed`.

### 9.5 The timing measurement (a separate build)

The measurement of how much logging delays a 1 kHz task (`TIMING` lines, from #13) no longer runs
in the normal firmware: it logs 1000 lines/s, which would go to flash (~2 MB per boot) and push
out the log of the previous boot, exactly what a crash left behind. It has its own build:

```powershell
cd firmware\examples\demo   # with the ESP-IDF 6.1.1 environment active
idf.py -B build-timing -D SDKCONFIG=build-timing/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.timing" -p COM6 flash
```

In that build it runs once per boot. With the flash on, the log task is slower than 1000 lines/s
and `dropped N lines: log queue full` lines appear: the excess is dropped and counted, never
blocking the application.

---

## How the pictures were made

1. `scripts/record_session.py <port> <seconds> <out.json> [--reset]` records, from a real
   session with the board, exactly what the serial transport delivered (bytes and notices) with
   the time of each. The recordings live in `docs/project/manual/sessions/`.
2. `scripts/manual_screenshots.py` runs each recording through the same pipeline and renderer
   as `esp-logcat stream` and exports the terminal window as SVG into `docs/project/manual/img/`. Cuts
   in long sessions show up as `...`. The `shell` pictures come from
   `scripts/record_shell.py <port> <out.json>`, which records real shell runs (the bytes
   received and the commands sent); the script replays them through the shell's own code
   (reply matching, blocks, `print_reply`).
3. `scripts/readme_cover.py` draws the README cover (`docs/project/img/cover.svg`) with real output
   from the same recordings.

When the output changes, record again and rerun the scripts: the pictures always show what
the tool really does.
