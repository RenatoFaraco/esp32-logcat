"""Classify one line of device output. Pure functions, no I/O.

A line is one of:
- ``LogLine``: the esp32-logcat format (PRD §3), validated exactly like the firmware's C++
  ``parse_line`` (same rules, same check order, same rejection reasons).
- ``InvalidLine``: looks like that format but must not be trusted (torn, garbage...).
- ``IdfLine``: the ESP-IDF native log format (``I (253) TAG: msg``), used by the bootloader
  and by anything logged before the esp32-logcat hook is installed.
- ``CommandReply`` / ``DumpBegin`` / ``DumpEnd``: console protocol (ARCHITECTURE §6).
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
# Short torn heads: up to 4 digits, then ':' or the end (possibly closed by DEL + "\n").
_TORN_HEAD = re.compile(rb"\d{1,4}(?::|\x7f?\n?\Z)")
# Fixed separators of the header: ':' at 4 and spaces at 11, 22 and 24.
_SEPARATORS = ((4, ord(":")), (11, ord(" ")), (22, ord(" ")), (24, ord(" ")))
_IDF = re.compile(rb"([EWIDV]) \((\d+)\) ([^:\n]+): (.*)", re.DOTALL)
_DUMP_BEGIN = re.compile(rb"#BEGIN (\S+) (\d+)")
_DUMP_END = re.compile(rb"#END ([0-9a-fA-F]{8})")
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
    text: str


@dataclass(frozen=True, slots=True)
class DumpBegin:
    filename: str
    size: int


@dataclass(frozen=True, slots=True)
class DumpEnd:
    crc32: int


@dataclass(frozen=True, slots=True)
class RawLine:
    text: str


Line = LogLine | InvalidLine | IdfLine | CommandReply | DumpBegin | DumpEnd | RawLine


def parse_line(data: bytes, *, lenient: bool = False) -> Line:
    """Classify one line. Never raises: unknown input becomes a RawLine.

    ``lenient`` accepts ``TAG:`` directly followed by ``\\n`` in the esp32-logcat format, for
    fixtures edited by hand that lost the trailing space of an empty message. Device data must
    always be parsed strictly.
    """
    if _looks_like_log_line(data):
        return _parse_log_line(data, lenient)
    body = _ANSI.sub(b"", data.removesuffix(b"\n"))
    if body.startswith(b"#> "):
        return CommandReply(text=_decode(body[3:]))
    if match := _DUMP_BEGIN.fullmatch(body):
        return DumpBegin(filename=_decode(match[1]), size=int(match[2]))
    if match := _DUMP_END.fullmatch(body):
        return DumpEnd(crc32=int(match[1], 16))
    if data.endswith(b"\n") and (match := _IDF.fullmatch(body)):
        return IdfLine(
            level=match[1].decode("ascii"),
            timestamp_ms=int(match[2]),
            tag=_decode(match[3]),
            msg=_decode(match[4]),
        )
    return RawLine(text=_decode(body))


def _looks_like_log_line(data: bytes) -> bool:
    """True for esp32-logcat lines, valid or damaged, so damage is counted as InvalidLine.

    Recognized by structure, not only by the first bytes: a corrupted digit or ':' at the
    start must still be reported as a bad header, not silently shown as raw text. Three of the
    four fixed separators are enough; ESP-IDF and ROM lines practically never match them.
    """
    if _TORN_HEAD.match(data):
        return True
    if len(data) < _HEADER_LENGTH:
        return False
    return sum(data[index] == value for index, value in _SEPARATORS) >= 3


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
