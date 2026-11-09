"""Turn parsed lines into styled ``rich.text.Text``. Pure functions, no I/O (ADR-008).

The plain text of a rendered line is the original line without its ``\\n`` (and without the
ANSI codes the parser removes): colors change, content does not, so what ``grep`` finds on
screen is what is stored on the device. Known normalizations: leading zeros in an ESP-IDF
timestamp or a dump size, lowercase dump CRC, and ``TAG: `` for a ``TAG:`` read leniently.

Device text is data, never markup. Everything is added with ``Text.append`` and an explicit
style, so ``[bold]`` in a message is shown as is. Control and format characters (C0, DEL, C1,
a lone ESC, bidi overrides such as U+202E) are shown as ASCII escapes (``\\x1b``, ``\\u202E``)
so the device cannot drive the terminal. The escapes are ASCII on purpose: on Windows a piped
stdout is cp1252, where symbols such as ``␛`` raise ``UnicodeEncodeError``. Without colors an
escape looks like the same four characters typed literally (``\\x1b``); backslashes are not
escaped, so Windows paths in messages still read naturally.

The same ``Text`` serves ``stream``/``replay`` (printed by a ``Console``, which drops the
colors when stdout is not a terminal) and the TUI (#56).
"""

import unicodedata
from datetime import datetime
from itertools import groupby
from typing import assert_never

from rich.text import Text

from esp_logcat.events import Notice, NoticeKind
from esp_logcat.parser import (
    TRUNCATION_MARKER,
    CommandReply,
    DumpBegin,
    DumpEnd,
    IdfLine,
    InvalidLine,
    Line,
    LogLine,
    RawLine,
)

# adb logcat colors; the only place to tune the palette. Bright blue because plain blue is
# below 3:1 contrast on the default dark Windows Terminal scheme; Verbose keeps the terminal's
# own color (bright black is the background in Solarized Dark). Yellow is weak on light themes.
LEVEL_STYLES: dict[str, str] = {
    "E": "red",
    "W": "yellow",
    "I": "green",
    "D": "bright_blue",
    "V": "default",
}
HEADER_STYLE = "dim"
MARKER_STYLE = "dim"
PC_TIME_STYLE = "dim"
RAW_STYLE = "dim"
REPLY_STYLE = "cyan"
DUMP_STYLE = "dim magenta"
INVALID_STYLE = "red"
INVALID_LABEL_STYLE = "bold red reverse"
ESCAPE_STYLE = "dim reverse"

# Characters that act on the terminal or on the layout instead of being drawn, plus lone
# surrogates (from a surrogateescape decode), which no encoder can write.
_ESCAPED_CATEGORIES = frozenset({"Cc", "Cf", "Zl", "Zp", "Cs"})


def render(line: Line, *, pc_time: datetime | None = None) -> Text:
    """Render one parsed line; ``pc_time`` adds a ``HH:MM:SS.mmm`` column (PRD H6)."""
    text = Text()
    if pc_time is not None:
        text.append(pc_time.strftime("%H:%M:%S.%f")[:-3] + " ", style=PC_TIME_STYLE)
    match line:
        case LogLine():
            _render_log_line(text, line)
        case IdfLine():
            # Level color without bold: lines logged before the hook, told apart by their layout.
            style = _level_style(line.level)
            _append(text, line.level, style)
            text.append(f" ({line.timestamp_ms}) ", style=style)
            _append(text, line.tag, style)
            text.append(": ", style=style)
            _append(text, line.msg, style)
        case RawLine():
            _append(text, line.text, RAW_STYLE)
        case InvalidLine():
            text.append(f"!! {line.reason}:", style=INVALID_LABEL_STYLE)
            text.append(" ")
            raw = line.raw.removesuffix(b"\n").decode("latin-1")
            _append(text, raw, INVALID_STYLE, ascii_only=True)
        case CommandReply():
            text.append("#> ", style=REPLY_STYLE)
            _append(text, line.text, REPLY_STYLE)
        case DumpBegin():
            text.append("#BEGIN ", style=DUMP_STYLE)
            _append(text, line.filename, DUMP_STYLE)
            text.append(f" {line.size}", style=DUMP_STYLE)
        case DumpEnd():
            text.append(f"#END {line.crc32:08x}", style=DUMP_STYLE)
        case _:
            assert_never(line)
    return text


_NOTICE_STYLES: dict[NoticeKind, str] = {
    NoticeKind.CONNECTED: "dim",
    NoticeKind.WAITING: "dim",
    NoticeKind.MISSED: "dim",
    NoticeKind.DISCONNECTED: "yellow",
    NoticeKind.GAP: "yellow",
    NoticeKind.REORDER: "dim yellow",
    NoticeKind.RESTART: "yellow",
    NoticeKind.UPTIME_BACK: "yellow",
    NoticeKind.SEQ_BACK: "yellow",
    NoticeKind.OVERFLOW: "yellow",
    NoticeKind.FILTER: "dim",
}


def device_text(data: str, style: str = "") -> Text:
    """Any text that did not come from this program (device, OS), escaped like a line."""
    text = Text()
    _append(text, data, style)
    return text


def render_notice(notice: Notice) -> Text:
    """A notice about the stream (connection, continuity), as ``-- text --``."""
    style = _NOTICE_STYLES.get(notice.kind, "dim")
    text = Text()
    text.append("-- ", style=style)
    _append(text, notice.text, style)  # may quote exception text from the OS
    text.append(" --", style=style)
    return text


def _level_style(level: str) -> str:
    # The parser only produces EWIDV; anything else must not crash a running stream.
    return LEVEL_STYLES.get(level, "")


def _render_log_line(text: Text, line: LogLine) -> None:
    color = _level_style(line.level)
    text.append(f"{line.boot:04d}:{line.seq:06d} {line.uptime_ms:010d} ", style=HEADER_STYLE)
    # The parser already guarantees printable ASCII here; escaping again costs little and keeps
    # a LogLine built elsewhere (tests, a future source) from reaching the terminal unchecked.
    _append(text, line.level, f"bold {color}")
    text.append(" ")
    _append(text, line.tag, f"bold {color}")
    text.append(": ", style=color)
    if line.truncated and line.msg.endswith(TRUNCATION_MARKER):
        _append(text, line.msg.removesuffix(TRUNCATION_MARKER), color)
        text.append(TRUNCATION_MARKER, style=MARKER_STYLE)
    else:
        _append(text, line.msg, color)


def _append(text: Text, data: str, style: str, *, ascii_only: bool = False) -> None:
    """Append device text with `style`, escaping what must not reach the terminal."""
    if data.isprintable() and (data.isascii() or not ascii_only):
        text.append(data, style=style)
        return
    # One span per run: a line of line noise becomes a few spans, not one per byte.
    for escaped, run in groupby(data, key=lambda char: _needs_escape(char, ascii_only)):
        if escaped:
            text.append("".join(_escape(char) for char in run), style=ESCAPE_STYLE)
        else:
            text.append("".join(run), style=style)


def _needs_escape(char: str, ascii_only: bool) -> bool:
    if ascii_only and not " " <= char <= "~":
        return True
    return unicodedata.category(char) in _ESCAPED_CATEGORIES


def _escape(char: str) -> str:
    code = ord(char)
    if code <= 0xFF:
        return f"\\x{code:02x}"
    if code <= 0xFFFF:
        return f"\\u{code:04X}"
    return f"\\U{code:08X}"
