"""Per-session view of the stream: counters and seq/uptime continuity. Pure, no I/O.

The firmware numbers every line it keeps (seq, mod 1e6, assigned after the tag filter) so the
host can tell lost lines from filtered ones (ADR-004). Rules, in order, for each LogLine
against the last one accepted:
- a ROM banner (``ESP-ROM:``) came in between, or boot changed → restart (boot stays 0 until
  M4, so a boot loop is only visible through the banner both ports receive);
- seq is the next one → in order (uptime going back more than a second here hints at a
  corrupted line, since there is no per-line checksum: ADR-007);
- uptime went back more than a second → restart;
- seq up to 32 behind (the firmware queue depth) → written out of order; if that seq had been
  reported lost, the loss is undone;
- seq more than 32 behind → restart if uptime went back at all, else a corrupted seq (a bad
  digit, ADR-007): reported, never counted as a loss of ~1e6 lines;
- otherwise seq jumped ahead → that many lines lost.
Each connection starts continuity over: lines the device wrote while no one was reading were
never sent (the firmware reports them itself), so a jump across a reconnect is not a loss. The
first line of a new connection is still compared with the last one before it, only to catch a
restart: on the native USB the reset button drops the port, and the ROM banner is printed before
the host can reopen it. Boot changed, uptime went back or seq did not move forward → restart.
A jump right after a line that may have been sitting
in the native port's FIFO (``maybe_stale``, decided by the pipeline) is "missed before the
connection", not "lost" (docs/hardware.md, item 7).
"""

from collections import Counter, deque

from esp_logcat.events import Notice, NoticeKind
from esp_logcat.parser import UPTIME_MAX, InvalidLine, Line, LogLine, RawLine

SEQ_MODULO = 1_000_000
REORDER_WINDOW = 32  # depth of the firmware's log queue (ARCHITECTURE §8)
UPTIME_TOLERANCE_MS = 1000
ROM_BANNER = "ESP-ROM:"
_UPTIME_WRAP_MARGIN_MS = 60_000


def _lines(count: int) -> str:
    return f"{count} line{'s' if count != 1 else ''}"


class StreamStats:
    def __init__(self) -> None:
        self.kinds: Counter[str] = Counter()
        self.invalid: Counter[str] = Counter()
        self.lost = 0
        self.missed = 0
        self.reordered = 0
        self.restarts = 0
        self.uptime_back = 0
        self.seq_back = 0
        self.reconnects = 0
        self.overflows = 0
        # Display filter (pipeline.visible): lines hidden from the output, still counted above.
        self.filtering = False
        self.hidden = 0
        self._connections = 0
        self._last: LogLine | None = None
        self._before_reconnect: LogLine | None = None  # last line of the previous connection
        self._last_maybe_stale = False
        self._booted = False  # a ROM banner since the last LogLine
        # Seqs recently reported lost: a late arrival (reorder) undoes the loss.
        self._recent_lost: deque[int] = deque(maxlen=REORDER_WINDOW)

    def connected(self) -> None:
        self._connections += 1
        if self._connections > 1:
            self.reconnects += 1
        if self._last is not None:
            self._before_reconnect = self._last
        self._last = None
        self._recent_lost.clear()

    def overflow(self) -> None:
        self.overflows += 1

    def observe(self, line: Line, *, maybe_stale: bool = False) -> list[Notice]:
        self.kinds[type(line).__name__] += 1
        if isinstance(line, InvalidLine):
            self.invalid[line.reason] += 1
            return []
        if isinstance(line, RawLine) and line.text.startswith(ROM_BANNER):
            self._booted = True
            return []
        if not isinstance(line, LogLine):
            return []
        last, last_maybe_stale = self._last, self._last_maybe_stale
        self._last, self._last_maybe_stale = line, maybe_stale
        booted, self._booted = self._booted, False
        if last is None:
            before, self._before_reconnect = self._before_reconnect, None
            if before is not None and (booted or _restarted_meanwhile(before, line)):
                return [self._restart(before, line)]
            return []
        if booted:
            return [self._restart(last, line)]
        return self._continuity(last, line, last_maybe_stale)

    def _continuity(self, last: LogLine, line: LogLine, last_maybe_stale: bool) -> list[Notice]:
        ahead = (line.seq - last.seq) % SEQ_MODULO
        behind = (last.seq - line.seq) % SEQ_MODULO
        went_back = _uptime_went_back(last.uptime_ms, line.uptime_ms)
        if line.boot != last.boot or (ahead != 1 and went_back):
            return [self._restart(last, line)]
        if ahead == 1:
            if went_back:
                self.uptime_back += 1
                return [
                    Notice(
                        NoticeKind.UPTIME_BACK,
                        f"uptime went back {last.uptime_ms - line.uptime_ms} ms at seq "
                        f"{line.seq:06d} (corrupted line?)",
                    )
                ]
            return []
        if behind <= REORDER_WINDOW:
            self._last, self._last_maybe_stale = last, last_maybe_stale  # newest seq stays
            self.reordered += 1
            note = ""
            if line.seq in self._recent_lost:
                self._recent_lost.remove(line.seq)
                self.lost -= 1
                note = " (not lost after all)"
            return [
                Notice(NoticeKind.REORDER, f"seq {line.seq:06d} arrived after {last.seq:06d}{note}")
            ]
        if ahead > SEQ_MODULO // 2:  # went back beyond the reorder window
            if line.uptime_ms < last.uptime_ms:
                return [self._restart(last, line)]
            self._last, self._last_maybe_stale = last, last_maybe_stale  # trust the older one
            self.seq_back += 1
            return [
                Notice(
                    NoticeKind.SEQ_BACK,
                    f"seq went back from {last.seq:06d} to {line.seq:06d} with uptime "
                    "going forward (corrupted line?)",
                )
            ]
        missing = ahead - 1
        if last_maybe_stale:
            self.missed += missing
            return [
                Notice(
                    NoticeKind.MISSED,
                    f"{_lines(missing)} written before the connection not received "
                    f"(up to seq {line.seq:06d})",
                )
            ]
        self.lost += missing
        for offset in range(max(1, missing - REORDER_WINDOW + 1), missing + 1):
            self._recent_lost.append((last.seq + offset) % SEQ_MODULO)
        return [Notice(NoticeKind.GAP, f"{_lines(missing)} lost before seq {line.seq:06d}")]

    def _restart(self, last: LogLine, line: LogLine) -> Notice:
        self.restarts += 1
        self._recent_lost.clear()
        return Notice(
            NoticeKind.RESTART,
            f"device restarted (boot {last.boot:04d} seq {last.seq:06d} -> "
            f"boot {line.boot:04d} seq {line.seq:06d})",
        )

    def summary(self) -> str:
        total = sum(self.kinds.values())
        parts = [
            f"{total} lines",
            f"{self.kinds['LogLine']} log",
            f"{self.kinds['IdfLine']} esp-idf",
            f"{self.kinds['RawLine']} raw",
            f"{self.kinds['InvalidLine']} invalid",
        ]
        if self.invalid:
            reasons = ", ".join(
                f"{reason} {count}" for reason, count in sorted(self.invalid.items())
            )
            parts[-1] += f" ({reasons})"
        events = [
            f"lost {self.lost}",
            f"missed before connecting {self.missed}",
            f"reordered {self.reordered}",
            f"restarts {self.restarts}",
            f"uptime back {self.uptime_back}",
            f"seq back {self.seq_back}",
            f"reconnects {self.reconnects}",
            f"overflows {self.overflows}",
        ]
        summary = ", ".join(parts) + " | " + ", ".join(events)
        if self.filtering:
            summary += f" | {self.hidden} hidden by filter"
        return summary


def _restarted_meanwhile(before: LogLine, line: LogLine) -> bool:
    """Across a reconnect: no reorder is possible (unsent lines are dropped, not kept)."""
    ahead = (line.seq - before.seq) % SEQ_MODULO
    return (
        line.boot != before.boot
        or _uptime_went_back(before.uptime_ms, line.uptime_ms)
        or ahead == 0
        or ahead > SEQ_MODULO // 2
    )


def _uptime_went_back(previous: int, current: int) -> bool:
    if previous > UPTIME_MAX - _UPTIME_WRAP_MARGIN_MS and current < _UPTIME_WRAP_MARGIN_MS:
        return False  # 32-bit millisecond uptime wraps after ~49.7 days
    return previous - current > UPTIME_TOLERANCE_MS
