"""What the stream pipeline produces: device lines and notices about the stream itself."""

from dataclasses import dataclass
from datetime import datetime
from enum import StrEnum

from esp_logcat.parser import Line


class NoticeKind(StrEnum):
    """Events about the connection or the continuity of the stream, not device output."""

    CONNECTED = "connected"
    DISCONNECTED = "disconnected"
    WAITING = "waiting"  # port absent or busy; retrying
    GAP = "gap"  # seq jumped ahead: lines lost (queue full on the device, or the wire)
    MISSED = "missed"  # first jump after connecting: written before we listened, not a loss
    REORDER = "reorder"  # seq slightly behind: written out of order (ADR-004 race)
    RESTART = "restart"  # seq and uptime went back together, or boot changed
    UPTIME_BACK = "uptime_back"  # seq continuous but uptime went back: corrupted line?
    SEQ_BACK = "seq_back"  # seq far behind with uptime going forward: corrupted seq?
    OVERFLOW = "overflow"  # no newline for too long: line cut, rest discarded
    SAVE_FAILED = "save_failed"  # --save hit a write error; the stream goes on
    FILTER = "filter"  # the active display filter (first notice when one is set)
    SOURCE = "source"  # what a non-serial stream reads (a file in replay); not a connection


@dataclass(frozen=True, slots=True)
class Notice:
    kind: NoticeKind
    text: str


@dataclass(frozen=True, slots=True)
class Received:
    line: Line
    pc_time: datetime | None  # None: unknown (a plain log replayed)


Event = Received | Notice
