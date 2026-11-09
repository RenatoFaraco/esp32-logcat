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
from esp_logcat.stats import StreamStats
from esp_logcat.transport import MAX_PENDING, LineSplitter

# Where a new line visibly starts: an esp32-logcat header, or the ROM banner of a boot.
_LINE_START = re.compile(rb"\d{4}:\d{6} \d{10} [EWIDV] |ESP-ROM:")


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
) -> Iterator[Event]:
    splitter = LineSplitter()
    # Bytes since the last CONNECTED; None before any (a file has no FIFO).
    since_connect: int | None = None

    def received(raw_lines: list[bytes]) -> Iterator[Event]:
        nonlocal since_connect
        for raw in raw_lines:
            in_fifo = since_connect is not None and since_connect < FIFO_BYTES
            if since_connect is not None:
                since_connect += len(raw)
            # (bytes, may have been sitting in the FIFO)
            pieces = [(raw, in_fifo)]
            if in_fifo:
                head, *tail = split_glued(raw)
                pieces = [(head, True)] + [(piece, False) for piece in tail]  # tail is live
            for piece, maybe_stale in pieces:
                line = parse_line(piece)
                # Continuity notices describe what happened before this line: emit them first.
                yield from stats.observe(line, maybe_stale=maybe_stale)
                yield Received(line=line, pc_time=clock())

    for item in chunks:
        if isinstance(item, Notice):
            if item.kind is NoticeKind.DISCONNECTED:
                yield from received(splitter.flush())  # the tail is a torn line
            elif item.kind is NoticeKind.CONNECTED:
                stats.connected()
                since_connect = 0
            yield item
            continue
        overflows = splitter.overflows
        lines = splitter.feed(item)
        for _ in range(splitter.overflows - overflows):
            stats.overflow()
            yield Notice(
                NoticeKind.OVERFLOW,
                f"over {MAX_PENDING} bytes without a newline: line cut, rest discarded",
            )
        yield from received(lines)
    yield from received(splitter.flush())
