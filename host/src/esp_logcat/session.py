"""Session files (`--save`): every line the device sent, with the PC time next to it (PRD H6).

    # esp-logcat session v1
    # 2026-11-08T10:15:02.120 notice connected connected to COM6
    2026-11-08T10:15:03.410 0000:000107 0000001030 I HEARTBEAT: tick 1
    # 2026-11-08T10:15:09.002 raw 303030303a3030

- A device line: the PC's local time (date included, milliseconds) + a space + the exact bytes
  the line splitter produced, newline included: invalid and raw lines too, before any filter.
- `#` lines are metadata (a device line always starts with its timestamp); a file is read as a
  session only if its first line is the header, so `#` lines of a plain log stay device lines:
  - `notice <kind> <text>`: what the transport reported (connected, disconnected, waiting,
    overflow). Gaps, restarts and the like are not saved: replay computes them again;
  - `raw <hex>`: bytes that a plain record cannot carry unchanged: no final newline (a torn
    tail, a cut over 1024 bytes) or a carriage return (the splitter would rewrite "\\r\\n").
  - The time field is `-` when the PC time is unknown (a plain log replayed with --save).

Recording happens inside pipeline.events(), on the input of the processing (lines straight out
of the splitter and transport notices), so a replay runs the same input through the same code:
the screen and the summary come out the same. Plain text: readable in an editor, greppable.
No rich, no UI (ADR-008).
"""

import contextlib
import re
from collections.abc import Callable
from datetime import datetime
from typing import BinaryIO

from esp_logcat.events import Notice, NoticeKind

HEADER = b"# esp-logcat session v1\n"
TIME_FORMAT = "%Y-%m-%dT%H:%M:%S.%f"
TIME_LENGTH = 23  # 2026-11-08T10:15:02.120
UNKNOWN_TIME = "-"
# Transport notices are input to the pipeline; the others are computed from the lines.
SAVED_NOTICES = frozenset(
    {NoticeKind.CONNECTED, NoticeKind.DISCONNECTED, NoticeKind.WAITING, NoticeKind.OVERFLOW}
)
_TIMED_LINE = re.compile(rb"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3} ")


def format_time(moment: datetime) -> str:
    return moment.strftime(TIME_FORMAT)[:TIME_LENGTH]


def parse_time(text: bytes | str) -> datetime | None:
    try:
        value = text.decode("ascii") if isinstance(text, bytes) else text
        return datetime.strptime(value, TIME_FORMAT)
    except (UnicodeDecodeError, ValueError):
        return None


class SessionWriter:
    """Writes a session file, flushing every record (Ctrl+C or a crash keeps what was seen).

    A write error (disk full, drive removed) never stops the stream: the writer turns itself
    off and keeps the reason in `error`, which the pipeline reports once as a notice.
    """

    def __init__(self, file: BinaryIO, clock: Callable[[], datetime] = datetime.now) -> None:
        self._file = file
        self._clock = clock
        self.error: str | None = None
        self._write(HEADER)

    def line(self, raw: bytes, pc_time: datetime | None) -> None:
        stamp = format_time(pc_time) if pc_time is not None else UNKNOWN_TIME
        if raw.endswith(b"\n") and b"\r" not in raw and pc_time is not None:
            self._write(stamp.encode("ascii") + b" " + raw)
        else:
            self._write(f"# {stamp} raw {raw.hex()}\n".encode("ascii"))

    def notice(self, notice: Notice, pc_time: datetime | None = None) -> None:
        if notice.kind not in SAVED_NOTICES:
            return
        stamp = format_time(pc_time or self._clock())
        text = notice.text.replace("\r", " ").replace("\n", " ")
        self._write(f"# {stamp} notice {notice.kind.value} {text}\n".encode("utf-8", "replace"))

    def close(self) -> None:
        with contextlib.suppress(OSError):
            self._file.close()

    def _write(self, record: bytes) -> None:
        if self.error is not None:
            return
        try:
            self._file.write(record)
            self._file.flush()
        except OSError as exc:
            self.error = str(exc.strerror or exc)


def split_timed(raw: bytes) -> tuple[datetime | None, bytes] | None:
    """A saved device line: (PC time, the device bytes); None if it has no session timestamp."""
    if not _TIMED_LINE.match(raw):
        return None
    return parse_time(raw[:TIME_LENGTH]), raw[TIME_LENGTH + 1 :]


def parse_metadata(raw: bytes) -> tuple[str, datetime | None, Notice | bytes] | None:
    """A `#` record: ("notice", time, Notice) or ("raw", time, bytes); None to ignore it
    (the header, comments, unknown or damaged records)."""
    parts = raw.rstrip(b"\r\n").decode("utf-8", "replace").split(" ", 3)
    if len(parts) < 4 or parts[0] != "#":
        return None
    _, stamp, kind, rest = parts
    moment = None if stamp == UNKNOWN_TIME else parse_time(stamp)
    if kind == "notice":
        name, _, text = rest.partition(" ")
        try:
            notice_kind = NoticeKind(name)
        except ValueError:
            return None
        return "notice", moment, Notice(notice_kind, text)
    if kind == "raw":
        try:
            return "raw", moment, bytes.fromhex(rest)
        except ValueError:
            return None
    return None
