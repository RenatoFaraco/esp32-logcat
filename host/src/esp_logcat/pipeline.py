"""Device bytes to events: split, parse, track continuity. Shared by stream, replay and TUI.

No rich and no I/O of its own: the input is any iterable of byte chunks and transport
notices (SerialTransport.chunks(), or a file read in pieces), the output is ``Event``s.
"""

import re
from collections.abc import Callable, Iterable, Iterator
from datetime import datetime

from esp_logcat.events import Event, Notice, NoticeKind, Received
from esp_logcat.filters import LogFilter
from esp_logcat.parser import parse_line
from esp_logcat.session import HEADER, SessionWriter, parse_metadata, split_timed
from esp_logcat.stats import StreamStats
from esp_logcat.transport import MAX_PENDING, LineSplitter

# Where a new line visibly starts: an esp32-logcat header, or the ROM banner of a boot.
_LINE_START = re.compile(rb"\d{4}:\d{6} \d{10} [EWIDV] |ESP-ROM:")
# The PC time column of `stream --time` (render's HH:MM:SS.mmm prefix), in a saved file.
_PC_TIME = re.compile(rb"\d{2}:\d{2}:\d{2}\.\d{3} ")


# Bytes the native USB-Serial-JTAG FIFO keeps while nobody reads (docs/hardware.md, item 7).
FIFO_BYTES = 64


def split_glued(raw: bytes) -> list[bytes]:
    """Splits a line where a second line visibly starts (header or ROM banner).

    While nobody reads the native USB port, its 64-byte hardware FIFO keeps the first bytes
    written: short lines and the head of a line whose tail the device then dropped. They only
    leave the FIFO with the next write, so the head arrives glued to the first live line (or
    to the ROM banner after a reset): a valid-looking LogLine with a bogus message, and a
    false gap. Applied only to lines starting within the first 64 bytes after connecting; the
    head becomes a torn line of its own.
    """
    match = _LINE_START.search(raw, 1)
    if match is None:
        return [raw]
    return [raw[: match.start()], raw[match.start() :]]


def visible(
    source: Iterable[Event], stats: StreamStats, line_filter: LogFilter | None
) -> Iterator[Event]:
    """The display filter over events(), shared by stream, replay and the TUI.

    Announces the active filter with a FILTER notice, drops the device lines it rejects and
    counts them in `stats.hidden`. events() has already updated the statistics, so hidden lines
    still count for gaps, restarts and the summary: hiding a tag never looks like lost lines.
    Notices are never filtered.
    """
    if line_filter is None or line_filter.is_trivial:
        yield from source
        return
    stats.filtering = True
    yield Notice(NoticeKind.FILTER, f"filter: {line_filter.describe()}")
    for event in source:
        if isinstance(event, Received) and not line_filter.accepts(event.line):
            stats.hidden += 1
            continue
        yield event


def events(
    chunks: Iterable[bytes | Notice],
    stats: StreamStats,
    *,
    clock: Callable[[], datetime] = datetime.now,
    lenient: bool = False,
    saved_file: bool = False,
    recorder: SessionWriter | None = None,
) -> Iterator[Event]:
    """Bytes and transport notices to events.

    ``lenient`` is passed to parse_line: only for files edited by hand (replay --lenient);
    device data is always parsed strictly.

    ``saved_file`` (replay) reads what a file may carry around the device bytes. A session
    (`--save`, recognized by its header line): saved PC times become the lines' pc_time, saved
    notices are emitted again with their live effects, raw records come back byte for byte.
    Otherwise a plain log: the HH:MM:SS.mmm column of `stream --time > file` is dropped, lines
    get pc_time None, and lines over 1024 bytes are cut like the live splitter does. With
    ``lenient``, a last line without its newline is accepted.

    ``recorder`` saves the input as it enters the processing (each splitter line with its PC
    time, and the transport notices), so a replay of the session runs the same input through
    the same code and shows the same thing. A write error is reported once as SAVE_FAILED.
    """
    # Session records can be longer than device lines (a cut 1024-byte line is 2 KiB of hex):
    # a saved file is split with a wide cap, and plain-log lines are cut at 1024 in received().
    splitter = LineSplitter(MAX_PENDING * 64 if saved_file else MAX_PENDING)
    # Bytes since the last CONNECTED; None before any (a file has no FIFO).
    since_connect: int | None = None
    first_line = True
    session = False
    save_failure_reported = False

    def save_failure() -> Iterator[Event]:
        nonlocal save_failure_reported
        if recorder is not None and recorder.error is not None and not save_failure_reported:
            save_failure_reported = True
            yield Notice(NoticeKind.SAVE_FAILED, f"save stopped: {recorder.error}")

    def notice(
        item: Notice, pc_time: datetime | None = None, *, from_file: bool = False
    ) -> Iterator[Event]:
        nonlocal since_connect
        if item.kind is NoticeKind.DISCONNECTED and not from_file:
            # The torn tail first, then the notice: the order the screen shows it in. (In a
            # session file the tail already has its own raw record, recorded before this.)
            yield from received(splitter.flush())
        if recorder is not None:
            recorder.notice(item, pc_time)
            yield from save_failure()
        if item.kind is NoticeKind.CONNECTED:
            stats.connected()
            since_connect = 0
        elif item.kind is NoticeKind.OVERFLOW:
            stats.overflow()
        yield item

    def overflow_notice() -> Notice:
        return Notice(
            NoticeKind.OVERFLOW,
            f"over {MAX_PENDING} bytes without a newline: line cut, rest discarded",
        )

    def received(raw_lines: list[bytes]) -> Iterator[Event]:
        nonlocal first_line, session
        for raw in raw_lines:
            pc_time: datetime | None = None
            if saved_file:
                if first_line:
                    first_line = False
                    session = raw == HEADER
                if session and raw.startswith(b"#"):
                    yield from metadata(raw)
                    continue
                timed = split_timed(raw) if session else None
                if timed is not None:
                    pc_time, raw = timed
                elif _PC_TIME.match(raw):
                    raw = raw[13:]  # saved by `stream --time`: the PC time is not device data
                if not session and len(raw.rstrip(b"\n")) > MAX_PENDING:
                    yield from notice(overflow_notice())
                    raw = raw[:MAX_PENDING]  # what the live splitter would have kept
            else:
                pc_time = clock()
            yield from line(raw, pc_time)

    def metadata(raw: bytes) -> Iterator[Event]:
        record = parse_metadata(raw)
        if record is None:
            return  # header, comment, or something this version does not know
        kind, pc_time, payload = record
        if kind == "notice":
            yield from notice(payload, pc_time, from_file=True)
        else:
            yield from line(payload, pc_time)

    def line(raw: bytes, pc_time: datetime | None) -> Iterator[Event]:
        nonlocal since_connect
        if recorder is not None:
            recorder.line(raw, pc_time)
            yield from save_failure()
        in_fifo = since_connect is not None and since_connect < FIFO_BYTES
        if since_connect is not None:
            since_connect += len(raw)
        # (bytes, may have been sitting in the FIFO)
        pieces = [(raw, in_fifo)]
        if in_fifo:
            head, *tail = split_glued(raw)
            pieces = [(head, True)] + [(piece, False) for piece in tail]  # tail is live
        for piece, maybe_stale in pieces:
            parsed = parse_line(piece, lenient=lenient)
            # Continuity notices describe what happened before this line: emit them first.
            yield from stats.observe(parsed, maybe_stale=maybe_stale)
            yield Received(line=parsed, pc_time=pc_time)

    for item in chunks:
        if isinstance(item, Notice):
            yield from notice(item)
            continue
        overflows = splitter.overflows
        lines = splitter.feed(item)
        for _ in range(splitter.overflows - overflows):
            yield from notice(overflow_notice())
        yield from received(lines)
    tail = splitter.flush()
    if lenient and saved_file:
        # A file edited by hand often ends without a final newline: not a torn line.
        tail = [raw + b"\n" for raw in tail]
    yield from received(tail)
