"""Classify one line of device output. Pure functions, no I/O.

A line is one of:
- ``LogLine``: the esp32-logcat format (wire-format §1), validated exactly like the firmware's C++
  ``parse_line`` (same rules, same check order, same rejection reasons).
- ``InvalidLine``: looks like that format but must not be trusted (torn, garbage...).
- ``IdfLine``: the ESP-IDF native log format (``I (253) TAG: msg``), used by the bootloader
  and by anything logged before the esp32-logcat hook is installed.
- ``CommandReply`` / ``CommandEnd`` / ``DumpBegin`` / ``DumpEnd``: console replies, with the
  request id (ADR-012). Exact or nothing: a malformed protocol line is a ``RawLine``.
- ``RawLine``: anything else (ROM messages, a stale fragment from the native USB port...).

Input is the raw bytes of one line including its final ``\\n`` (absent only for an
unterminated tail). Serial transports must turn the console's ``\\r\\n`` into ``\\n`` first:
the strict format, like the firmware, treats ``\\r`` as garbage.
"""

import re
from dataclasses import dataclass
from enum import StrEnum

MAX_LINE_LENGTH = 170
MAX_TAG_LENGTH = 15
MAX_MESSAGE_LENGTH = 127
UPTIME_MAX = 0xFFFF_FFFF
LEVELS = "EWIDV"
TRUNCATION_MARKER = "..."
TORN_SENTINEL = 0x7F  # DEL: the store closes a torn tail with DEL + "\n" (#23)

_HEADER_LENGTH = 25  # "BBBB:SSSSSS UUUUUUUUUU L "
_MIN_LINE_LENGTH = _HEADER_LENGTH + 1 + 2 + 1  # 1-char tag, ": ", "\n"

# In a bytes pattern \d only matches ASCII 0-9: no Unicode digits, signs or spaces.
_HEADER = re.compile(rb"(\d{4}):(\d{6}) (\d{10}) (.) ", re.DOTALL)
# Header layout as a template: D = ASCII digit, L = level letter (any byte here; validated
# later), other bytes literal. Used to recognize short torn heads such as "0042:0001".
_HEADER_TEMPLATE = b"DDDD:DDDDDD DDDDDDDDDD L "
# Fixed separators of the header: ':' at 4 and spaces at 11, 22 and 24.
_SEPARATORS = ((4, ord(":")), (11, ord(" ")), (22, ord(" ")), (24, ord(" ")))
# A full header after a few bytes of junk (UART noise right after a reset).
_JUNK_THEN_HEADER = re.compile(rb".{1,8}?\d{4}:\d{6} \d{10} . ", re.DOTALL)
# Bounded digit counts: int() on thousands of digits raises (Python's 4300-digit limit).
_IDF = re.compile(rb"([EWIDV]) \((\d{1,10})\) ([^:\n]+): (.*)", re.DOTALL)
# Console replies (ADR-012). The id is echoed from the request: 1-9999, or "-" when typed by hand.
_ID = rb"(-|[1-9][0-9]{0,3})"
_FIELD = rb"[a-z][a-z0-9_.]{0,31}=[\x21-\x7e]+"  # value: printable ASCII without space
_REPLY_DATA = re.compile(rb"#> " + _ID + rb" (" + _FIELD + rb"(?: " + _FIELD + rb")*)")
_REPLY_OK = re.compile(rb"#\. " + _ID + rb" ok")
# Error text: optional, starts with a non-space (a trailing space after the code is malformed).
_REPLY_ERROR = re.compile(rb"#! " + _ID + rb" ([a-z_]{1,32})(?: ([\x21-\x7e][\x20-\x7e]*))?")
# A label, not a path: no separators, no dot-files. The dump (#29) still picks the local file
# name itself (Windows reserves CON, NUL, COM1...).
_DUMP_BEGIN = re.compile(
    rb"#BEGIN " + _ID + rb" ([A-Za-z0-9_][A-Za-z0-9._-]{0,63}) (0|[1-9][0-9]{0,9})"
)
_DUMP_END = re.compile(rb"#END " + _ID + rb" ([0-9a-fA-F]{8})")
_ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


class InvalidReason(StrEnum):
    """Why a line in the esp32-logcat format is rejected (names match the firmware)."""

    TORN = "torn"
    GARBAGE = "garbage"
    BAD_HEADER = "bad_header"
    BAD_LEVEL = "bad_level"
    OVERLONG = "overlong"


@dataclass(frozen=True, slots=True)
class LogLine:
    boot: int
    seq: int
    uptime_ms: int
    level: str
    tag: str
    msg: str  # raw: edge spaces and a final "..." kept, so the line reprints identically
    truncated: bool


@dataclass(frozen=True, slots=True)
class InvalidLine:
    reason: InvalidReason
    raw: bytes


@dataclass(frozen=True, slots=True)
class IdfLine:
    level: str
    timestamp_ms: int
    tag: str
    msg: str


@dataclass(frozen=True, slots=True)
class CommandReply:
    """``#> <id> <key=value ...>``: one data line of a console reply (ADR-012)."""

    id: str
    fields: tuple[tuple[str, str], ...]  # in wire order, each key once

    def get(self, key: str) -> str | None:
        """The value, or None when the device did not report it: unknown, never zero."""
        for name, value in self.fields:
            if name == key:
                return value
        return None


@dataclass(frozen=True, slots=True)
class CommandEnd:
    """``#. <id> ok`` or ``#! <id> <code> [text]``: the end of a console reply (ADR-012)."""

    id: str
    code: str  # "ok" on success; an error code otherwise (unknown codes are kept as they are)
    text: str = ""  # for people only: no tool decides anything from it

    @property
    def ok(self) -> bool:
        return self.code == "ok"


@dataclass(frozen=True, slots=True)
class DumpBegin:
    id: str
    filename: str
    size: int


@dataclass(frozen=True, slots=True)
class DumpEnd:
    id: str
    crc32: int


@dataclass(frozen=True, slots=True)
class RawLine:
    text: str


Line = LogLine | InvalidLine | IdfLine | CommandReply | CommandEnd | DumpBegin | DumpEnd | RawLine


def parse_line(data: bytes, *, lenient: bool = False, assume_log: bool = False) -> Line:
    """Classify one line. Never raises: unknown input becomes a RawLine.

    ``assume_log``: every line is esp32-logcat (the content of a dump block, which is read by
    byte count so it never contains the closing marker (ADR-012), a file read from flash, the
    shared C++/Python fixture). No heuristics: the result is a LogLine or an InvalidLine with
    exactly the firmware's categories, so garbage is counted instead of shown as raw text.

    ``lenient`` accepts ``TAG:`` directly followed by ``\\n`` in the esp32-logcat format, for
    fixtures edited by hand that lost the trailing space of an empty message. Device data must
    always be parsed strictly.
    """
    if assume_log:
        return _parse_log_line(data, lenient)
    complete = data.endswith(b"\n")
    body = data.removesuffix(b"\n")
    # Protocol lines first: an error text with spaces in the right places would otherwise pass
    # the damaged-log-line heuristic below, and a log line never starts with "#". They must be
    # complete and exact: a torn "#BEGIN 4 log.txt 4096" must not become a 40-byte dump, and
    # ANSI codes must not disguise them.
    if (
        complete
        and len(data) <= MAX_LINE_LENGTH
        and body.startswith(b"#")
        and (reply := _parse_protocol_line(body)) is not None
    ):
        return reply
    if _looks_like_log_line(data):
        return _parse_log_line(data, lenient)
    plain = _ANSI.sub(b"", body)
    if complete and (match := _IDF.fullmatch(plain)):
        return IdfLine(
            level=match[1].decode("ascii"),
            timestamp_ms=int(match[2]),
            tag=_decode(match[3]),
            msg=_decode(match[4]),
        )
    return RawLine(text=_decode(plain))


def _parse_protocol_line(body: bytes) -> Line | None:
    """A console reply line (ADR-012), or None when it is not exactly one."""
    if match := _REPLY_DATA.fullmatch(body):
        # Already validated: single spaces between fields, the first "=" ends each key.
        pairs = (item.partition(b"=") for item in match[2].split(b" "))
        fields = tuple((key.decode("ascii"), value.decode("ascii")) for key, _, value in pairs)
        if len({key for key, _ in fields}) != len(fields):
            return None  # a repeated key: which value would be the right one?
        return CommandReply(id=match[1].decode("ascii"), fields=fields)
    if match := _REPLY_OK.fullmatch(body):
        return CommandEnd(id=match[1].decode("ascii"), code="ok")
    if (match := _REPLY_ERROR.fullmatch(body)) and match[2] != b"ok":  # "ok" only via "#."
        text = match[3].decode("ascii") if match[3] is not None else ""
        return CommandEnd(id=match[1].decode("ascii"), code=match[2].decode("ascii"), text=text)
    if match := _DUMP_BEGIN.fullmatch(body):
        return DumpBegin(
            id=match[1].decode("ascii"), filename=match[2].decode("ascii"), size=int(match[3])
        )
    if match := _DUMP_END.fullmatch(body):
        return DumpEnd(id=match[1].decode("ascii"), crc32=int(match[2], 16))
    return None


def _looks_like_log_line(data: bytes) -> bool:
    """True for esp32-logcat lines, valid or damaged, so damage is counted as InvalidLine.

    Three cases, each strict enough not to flag ordinary text ("12:34:56 sync", "42"):
    - a full-size header with at least 3 of its 4 fixed separators (a corrupted digit or ':'
      is still counted as a bad header, not shown as raw text);
    - a torn line (no final newline, or closed with DEL) that is an exact prefix of a header;
    - a full header after up to 8 bytes of junk (UART noise after a reset).
    Known limitation: a byte lost at index 4 removes the ':' anchor and shifts every separator.
    """
    if len(data) >= _HEADER_LENGTH and (
        sum(data[index] == value for index, value in _SEPARATORS) >= 3
    ):
        return True
    torn = not data.endswith(b"\n") or data.endswith(b"\x7f\n")
    head = data.removesuffix(b"\n").removesuffix(b"\x7f")
    if torn and head and _is_header_prefix(head):
        return True
    return _JUNK_THEN_HEADER.match(data) is not None


def _is_header_prefix(head: bytes) -> bool:
    if len(head) >= _HEADER_LENGTH:
        return False
    for byte, expected in zip(head, _HEADER_TEMPLATE, strict=False):
        if expected == ord("D"):
            if not 0x30 <= byte <= 0x39:
                return False
        elif expected != ord("L") and byte != expected:
            return False
    return True


def _parse_log_line(data: bytes, lenient: bool) -> LogLine | InvalidLine:
    # Same checks, in the same order, as the firmware's parse_line: the order decides the
    # reason. The store's sentinel comes first, so a closed 169-byte torn tail (171 bytes)
    # is torn rather than overlong.
    length = len(data)
    if 2 <= length <= MAX_LINE_LENGTH + 1 and data[-2] == TORN_SENTINEL and data[-1] == 0x0A:
        return InvalidLine(InvalidReason.TORN, data)
    if length > MAX_LINE_LENGTH:
        return InvalidLine(InvalidReason.OVERLONG, data)
    if not data.endswith(b"\n"):
        return InvalidLine(InvalidReason.TORN, data)
    body = data[:-1]
    if any(byte < 0x20 or byte > 0x7E for byte in body):
        return InvalidLine(InvalidReason.GARBAGE, data)

    lenient_empty = lenient and body.endswith(b":")
    if length < _MIN_LINE_LENGTH and not (lenient_empty and length == _MIN_LINE_LENGTH - 1):
        return InvalidLine(InvalidReason.BAD_HEADER, data)
    header = _HEADER.fullmatch(body[:_HEADER_LENGTH])
    if header is None:
        return InvalidLine(InvalidReason.BAD_HEADER, data)
    uptime = int(header[3])  # safe: exactly 10 ASCII digits
    if uptime > UPTIME_MAX:
        return InvalidLine(InvalidReason.BAD_HEADER, data)
    level = header[4].decode("ascii")
    if level not in LEVELS:
        return InvalidLine(InvalidReason.BAD_LEVEL, data)

    rest = body[_HEADER_LENGTH:]
    colon = rest.find(b":")
    tag = rest[:colon] if colon >= 0 else rest
    if colon < 0 or b" " in tag or not 1 <= len(tag) <= MAX_TAG_LENGTH:
        return InvalidLine(InvalidReason.BAD_HEADER, data)
    after = rest[colon + 1 :]
    if after.startswith(b" "):
        message = after[1:]
    elif lenient and not after:
        message = b""
    else:
        return InvalidLine(InvalidReason.BAD_HEADER, data)
    if len(message) > MAX_MESSAGE_LENGTH:
        return InvalidLine(InvalidReason.OVERLONG, data)

    msg = message.decode("ascii")
    return LogLine(
        boot=int(header[1]),
        seq=int(header[2]),
        uptime_ms=uptime,
        level=level,
        tag=tag.decode("ascii"),
        msg=msg,
        truncated=len(msg) == MAX_MESSAGE_LENGTH and msg.endswith(TRUNCATION_MARKER),
    )


def _decode(raw: bytes) -> str:
    return raw.decode("utf-8", errors="replace")
