"""Render the terminal pictures of docs/MANUAL.md from sessions recorded on the board.

Usage (from the repository root, with the host venv):
    host/.venv/Scripts/python scripts/manual_screenshots.py            # all pictures
    host/.venv/Scripts/python scripts/manual_screenshots.py --list boot  # line indices

Each recorded session (docs/manual/sessions/*.json, made by scripts/record_session.py) goes
through the same code as `esp-logcat stream`: pipeline.events() for parsing and stats,
render()/render_notice() for colors, and the same summary. The result is printed into a rich
Console that records it and is exported as SVG (a terminal window), so the pictures are real
device output and can be regenerated whenever the output changes.
"""

import base64
import io
import json
import sys
from collections.abc import Iterator
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path

from esp_logcat.events import Notice, NoticeKind, Received
from esp_logcat.filters import LogFilter
from esp_logcat.parser import InvalidLine, InvalidReason
from esp_logcat.pipeline import events, visible
from esp_logcat.render import device_text, render, render_notice
from esp_logcat.stats import StreamStats
from esp_logcat.transport import FileTransport, PortInfo
from rich.console import Console
from rich.terminal_theme import MONOKAI
from rich.text import Text

ROOT = Path(__file__).resolve().parent.parent
SESSIONS = ROOT / "docs" / "manual" / "sessions"
IMAGES = ROOT / "docs" / "manual" / "img"
PROMPT = "PS C:\\dev\\esp32-logcat> "
WIDTH = 140  # a heartbeat line with --time (~138 columns) fits on one row


@dataclass
class Shot:
    name: str  # output file: img/<name>.svg
    title: str  # window title
    command: str  # what the reader types
    session: str = ""  # sessions/<session>.json ("" for ports)
    show_time: bool = False
    # Which output lines to keep, as (first, last) inclusive ranges over the rendered lines;
    # a dim "..." line marks what was left out. Empty = everything.
    keep: list[tuple[int, int]] = field(default_factory=list)
    summary: bool = True
    filters: list[str] = field(default_factory=list)  # logcat-style specs, as on the command line
    # A saved log replayed with FileTransport (path relative to the repository), instead of a
    # recorded session: the picture of `esp-logcat replay`.
    file: str = ""


def replay(path: Path) -> tuple[Iterator[bytes | Notice], datetime, list[float]]:
    session = json.loads(path.read_text(encoding="utf-8"))
    started = datetime.fromisoformat(session["started"])
    offsets: list[float] = []

    def chunks() -> Iterator[bytes | Notice]:
        for item in session["items"]:
            offsets.append(item["t"])
            if "notice" in item:
                yield Notice(NoticeKind(item["notice"]), item["text"])
            else:
                yield base64.b64decode(item["data"])

    return chunks(), started, offsets


def stream_lines(shot: Shot) -> list[Text]:
    if shot.file:
        transport = FileTransport((ROOT / shot.file).open("rb"), shot.file.replace("/", "\\"))
        source, started, offsets = transport.chunks(), datetime(2026, 11, 8), []
    else:
        source, started, offsets = replay(SESSIONS / f"{shot.session}.json")
    stats = StreamStats()

    def clock() -> datetime:
        return started + timedelta(seconds=offsets[-1] if offsets else 0)

    # The same pipeline as cli.run_stream: events() then the display filter, visible().
    line_filter = LogFilter.parse(shot.filters)
    lines: list[Text] = []
    last = None
    for event in visible(events(source, stats, clock=clock), stats, line_filter):
        last = event
        if isinstance(event, Received):
            lines.append(render(event.line, pc_time=event.pc_time if shot.show_time else None))
        else:
            lines.append(render_notice(event))
    # A recording ends by closing the port mid-line: that tail is an artifact of the
    # recording, not something the reader would see before pressing Ctrl+C.
    tail = last.line if isinstance(last, Received) else None
    if isinstance(tail, InvalidLine) and tail.reason is InvalidReason.TORN:
        lines.pop()
        stats.invalid["torn"] -= 1
        stats.kinds["InvalidLine"] -= 1
        if not stats.invalid["torn"]:
            del stats.invalid["torn"]
    lines = crop(lines, shot.keep)
    if shot.summary:
        lines.append(Text("^C", style="bold"))
        lines.append(Text(stats.summary(), style="dim"))
    return lines


def crop(lines: list[Text], keep: list[tuple[int, int]]) -> list[Text]:
    if not keep:
        return lines
    out: list[Text] = []
    for index, (first, last) in enumerate(keep):
        if index > 0 or first > 0:
            out.append(Text("...", style="dim"))
        out.extend(lines[first : last + 1])
    if keep[-1][1] < len(lines) - 1:
        out.append(Text("...", style="dim"))
    return out


def ports_lines() -> list[Text]:
    ports = json.loads((SESSIONS / "ports.json").read_text(encoding="utf-8"))
    lines = []
    for raw in ports:
        port = PortInfo(**raw)
        ids = f"{port.vid:04X}:{port.pid:04X}" if port.vid is not None else "-"
        kind = "native USB-Serial-JTAG" if port.is_native else ""
        serial_number = f"SER={port.serial_number}" if port.serial_number else ""
        fields = [port.device, ids, serial_number, kind, port.description]
        lines.append(device_text("  ".join(f for f in fields if f)))
    return lines


def save(shot: Shot, lines: list[Text]) -> None:
    console = Console(
        file=io.StringIO(),
        record=True,
        width=WIDTH,
        force_terminal=True,
        color_system="truecolor",
        highlight=False,
        emoji=False,
        soft_wrap=False,
    )
    prompt = Text(PROMPT, style="bold blue")
    prompt.append(shot.command, style="bold")
    console.print(prompt)
    for line in lines:
        console.print(line)  # wraps at WIDTH, like a terminal of that width would
    IMAGES.mkdir(parents=True, exist_ok=True)
    svg = console.export_svg(title=shot.title, theme=MONOKAI)
    # LF on every OS (save_svg would write CRLF on Windows and show up as a diff in git)
    (IMAGES / f"{shot.name}.svg").write_text(svg, encoding="utf-8", newline="\n")
    print(f"img/{shot.name}.svg: {len(lines)} lines")


SHOTS = [
    Shot("ports", "esp-logcat ports", "esp-logcat ports", summary=False),
    # Ranges are indices of the rendered lines of the recorded session (see list_lines()).
    Shot(
        "boot",
        "esp-logcat stream --reset",
        "esp-logcat stream --reset",
        "boot",
        keep=[(0, 13), (53, 63), (88, 98)],
    ),
    Shot(
        "burst",
        "esp-logcat stream --reset",
        "esp-logcat stream --reset",
        "boot",
        keep=[(60, 66), (85, 96)],
    ),
    Shot("time", "esp-logcat stream --time", "esp-logcat stream --time", "time", show_time=True),
    Shot("reopen", "esp-logcat stream", "esp-logcat stream", "reopen"),
    Shot("busy", "esp-logcat stream", "esp-logcat stream", "busy"),
    Shot(
        "replay",
        r'esp-logcat replay host\tests\fixtures\two_boots_esp32c3.log "*:W"',
        r'esp-logcat replay host\tests\fixtures\two_boots_esp32c3.log "*:W"',
        file="host/tests/fixtures/two_boots_esp32c3.log",
        filters=["*:W"],
        keep=[(0, 2), (12, 17), (27, 32)],
    ),
    Shot(
        "filter",
        'esp-logcat stream --reset "*:S" "HEARTBEAT:I" "LOGCAT:W"',
        'esp-logcat stream --reset "*:S" "HEARTBEAT:I" "LOGCAT:W"',
        "boot",
        filters=["*:S", "HEARTBEAT:I", "LOGCAT:W"],
        keep=[(0, 3), (12, 19)],
    ),
]


def list_lines(session: str) -> None:
    """Prints the rendered lines of a session with their indices, to choose `keep` ranges."""
    for index, line in enumerate(stream_lines(Shot("", "", "", session, summary=False))):
        print(index, line.plain[:110])


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "--list":
        list_lines(sys.argv[2])
        return
    for shot in SHOTS:
        save(shot, ports_lines() if shot.name == "ports" else stream_lines(shot))
    tui_shot()


def tui_shot() -> None:
    """img/tui.svg: the real TUI (Textual's own screenshot) replaying the two-boot fixture."""
    import asyncio

    from esp_logcat.tui.app import LogcatApp

    async def capture() -> None:
        transport = FileTransport(
            (ROOT / "host/tests/fixtures/two_boots_esp32c3.log").open("rb"), "two_boots_esp32c3.log"
        )
        app = LogcatApp(
            transport.chunks(),
            transport.close,
            label="replay two_boots_esp32c3.log",
            saved_file=True,
            line_filter=LogFilter.parse(["*:W", "HEARTBEAT:I"]),
        )
        async with app.run_test(size=(118, 30)) as pilot:
            for _ in range(100):
                await pilot.pause(0.05)
                if app.finished:
                    break
            await pilot.pause(0.2)
            svg = app.export_screenshot(title="esp-logcat tui")
            (IMAGES / "tui.svg").write_text(svg, encoding="utf-8", newline="\n")
            await pilot.press("q")

    asyncio.run(capture())
    print("img/tui.svg")


if __name__ == "__main__":
    main()
