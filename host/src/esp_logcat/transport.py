"""Serial input: bytes from the device split into lines, with reconnect. No rich, no UI.

Board facts this relies on are in docs/context/hardware.md ("Comportamentos que importam"):
- the CH340K bridge drives EN (reset) with RTS and GPIO9 (download mode) with DTR, so the port
  is opened with both lines released;
- Windows' usbser.sys (native USB-Serial-JTAG) only forwards the control lines when DTR is
  written, so DTR is written again after every RTS change;
- the ESP-IDF console sends CRLF;
- an EN reset drops the native USB port, which may come back under another COM number;
- the native port keeps up to 64 bytes in its FIFO while nobody reads; they arrive glued to
  the next write (handled by pipeline.split_glued, not here).
"""

import codecs
import contextlib
import errno
import os
import sys
import threading
import time
from collections.abc import Callable, Generator, Iterator, Sequence
from dataclasses import dataclass
from typing import BinaryIO, Protocol

import serial
from serial.tools import list_ports as _pyserial_list_ports

from esp_logcat.events import Notice, NoticeKind

# Longest run without "\n" kept as one line. Not the 170-byte line limit: the parser already
# flags any esp32-logcat line over 170 bytes as overlong, while printf or ESP-IDF output may
# legitimately be longer. This only bounds memory on noise without newlines (wrong baud rate).
MAX_PENDING = 1024

NATIVE_USB = (0x303A, 0x1001)  # Espressif USB-Serial-JTAG
RESET_PULSE_S = 0.2


class LineSplitter:
    """Turns a byte stream into lines ending in ``\\n``. Pure: no I/O.

    ``\\r\\n`` becomes ``\\n``; a lone ``\\r`` is kept (the strict parser counts it as garbage,
    like the firmware). A line longer than ``max_pending`` bytes is cut: the first
    ``max_pending`` bytes are returned without ``\\n`` and the rest is discarded up to the next
    newline. The result never depends on how the bytes were chunked.
    """

    def __init__(self, max_pending: int = MAX_PENDING) -> None:
        self._max = max_pending
        self._pending = bytearray()
        self._discarding = False
        self.overflows = 0

    def feed(self, chunk: bytes) -> list[bytes]:
        lines: list[bytes] = []
        start = 0
        while (newline := chunk.find(b"\n", start)) >= 0:
            piece = chunk[start : newline + 1]
            start = newline + 1
            if self._discarding:
                self._discarding = False
                continue
            self._pending += piece
            line = bytes(self._pending)
            self._pending.clear()
            if line.endswith(b"\r\n"):
                line = line[:-2] + b"\n"
            if len(line) - 1 > self._max:
                lines.append(line[: self._max])
                self.overflows += 1
            else:
                lines.append(line)
        if not self._discarding:
            self._pending += chunk[start:]
            # A final '\r' may be half of a "\r\n" still to come: it does not count yet.
            if len(self._pending) - self._pending.endswith(b"\r") > self._max:
                lines.append(bytes(self._pending[: self._max]))
                self._pending.clear()
                self._discarding = True
                self.overflows += 1
        return lines

    def flush(self) -> list[bytes]:
        """Returns the unterminated tail (a torn line) and starts over."""
        tail = bytes(self._pending)
        self._pending.clear()
        self._discarding = False
        return [tail] if tail else []


@dataclass(frozen=True, slots=True)
class PortInfo:
    device: str
    vid: int | None = None
    pid: int | None = None
    serial_number: str | None = None
    location: str | None = None
    description: str = ""

    @property
    def is_native(self) -> bool:
        return (self.vid, self.pid) == NATIVE_USB

    def same_device(self, other: "PortInfo") -> bool:
        """Same physical device: serial number when there is one (native USB: the MAC), else
        the USB location (CH340K has no serial number, so the same hub/PC port)."""
        if (self.vid, self.pid) != (other.vid, other.pid) or self.vid is None:
            return False
        if self.serial_number:
            return self.serial_number == other.serial_number
        return self.location is not None and self.location == other.location


def list_ports() -> list[PortInfo]:
    return [
        PortInfo(
            device=port.device,
            vid=port.vid,
            pid=port.pid,
            serial_number=port.serial_number or None,
            location=port.location or None,
            description=port.description or "",
        )
        for port in _pyserial_list_ports.comports()
    ]


class PortSelectionError(Exception):
    pass


def find_port(ports: Sequence[PortInfo]) -> str:
    """The port to use without --port: the only native USB port, else the only port."""
    native = [port for port in ports if port.is_native]
    if len(native) == 1:
        return native[0].device
    if not native and len(ports) == 1:
        return ports[0].device
    listing = ", ".join(port.device for port in ports) or "none"
    raise PortSelectionError(f"cannot choose a port (found: {listing}); use --port")


class SerialLike(Protocol):
    """The part of serial.Serial the transport uses (tests provide a fake)."""

    port: str | None
    baudrate: int
    timeout: float | None
    dtr: bool
    rts: bool

    @property
    def in_waiting(self) -> int: ...

    def open(self) -> None: ...

    def close(self) -> None: ...

    def read(self, size: int) -> bytes: ...

    def write(self, data: bytes) -> int | None: ...


def set_rts(link: SerialLike, state: bool) -> None:
    # usbser.sys only sends SET_CONTROL_LINE_STATE when DTR is written (esptool does the same).
    link.rts = state
    link.dtr = link.dtr


def pulse_reset(link: SerialLike, sleep: Callable[[float], None] = time.sleep) -> None:
    """Resets the chip: EN on the CH340K bridge, a chip reset on the native USB port."""
    link.dtr = False
    set_rts(link, True)
    try:
        sleep(RESET_PULSE_S)
    finally:
        set_rts(link, False)  # even on Ctrl+C: RTS left on holds EN low on the CH340K


class SerialTransport:
    """Yields chunks of bytes from a port, and notices when it connects, drops or waits.

    Never gives up: an absent or busy port is retried every ``retry_interval`` seconds, and a
    port lost while reading is reopened, under a new COM number if the same device shows up
    there. Stops when ``close()`` is called or the consumer stops iterating.
    """

    def __init__(
        self,
        port: str,
        baudrate: int = 115200,
        *,
        reset: bool = False,
        open_serial: Callable[[], SerialLike] = serial.Serial,
        list_ports: Callable[[], list[PortInfo]] = list_ports,
        sleep: Callable[[float], None] = time.sleep,
        retry_interval: float = 0.5,
        read_timeout: float = 0.1,
    ) -> None:
        self.port = port
        self._requested = port
        self._baudrate = baudrate
        self._reset_pending = reset
        self._open_serial = open_serial
        self._list_ports = list_ports
        self._sleep = sleep
        self._retry_interval = retry_interval
        self._read_timeout = read_timeout
        self._identity: PortInfo | None = None
        self._closed = False
        # The open link, for write() from another thread (the shell); None while waiting.
        self._link: SerialLike | None = None
        self._write_lock = threading.Lock()

    def close(self) -> None:
        self._closed = True

    @property
    def connected(self) -> bool:
        return self._link is not None

    def write(self, data: bytes) -> bool:
        """Sends `data` to the device, from any thread. False when the port is not open (waiting
        or reconnecting) or the write fails: the caller decides what that means."""
        with self._write_lock:
            link = self._link
            if link is None:
                return False
            try:
                link.write(data)
            except (serial.SerialException, OSError):
                return False
            return True

    def chunks(self) -> Iterator[bytes | Notice]:
        while not self._closed:
            opened = yield from self._connect()
            if opened is None:
                return
            link, connected = opened
            lost = False
            # Everything after open() is inside try/finally, so the port is closed on every
            # path: read error, close(), the consumer stopping (GeneratorExit), Ctrl+C.
            try:
                with self._write_lock:
                    self._link = link
                yield connected
                if self._reset_pending:
                    self._reset_pending = False
                    pulse_reset(link, self._sleep)
                while not self._closed:
                    data = link.read(max(1, link.in_waiting))
                    if data:
                        yield data
            except (serial.SerialException, OSError) as exc:
                lost = True
                yield Notice(NoticeKind.DISCONNECTED, f"{self.port}: connection lost ({exc})")
            finally:
                with self._write_lock:
                    self._link = None
                _close_quietly(link)
            if lost:
                # A device being removed may still open and then fail at once: do not spin.
                self._sleep(self._retry_interval)

    def _connect(self) -> Generator[Notice, None, tuple[SerialLike, Notice] | None]:
        # Yields WAITING notices; returns the open link and its CONNECTED notice (yielded by
        # chunks() under its try/finally), or None once closed.
        last_reason = None
        while not self._closed:
            name = self._resolve_name()
            if name is None:
                reason = f"{self._requested} now belongs to another device"
                if reason != last_reason:
                    yield Notice(NoticeKind.WAITING, f"waiting for the device: {reason}")
                    last_reason = reason
                self._sleep(self._retry_interval)
                continue
            try:
                link = self._open(name)
            except (serial.SerialException, OSError) as exc:
                reason = _open_failure(exc)
                if reason != last_reason:
                    yield Notice(NoticeKind.WAITING, f"waiting for {name}: {reason}")
                    last_reason = reason
                self._sleep(self._retry_interval)
                continue
            if self._identity is None:
                self._identity = self._port_info(name)
            moved = f" (was {self.port})" if name != self.port else ""
            self.port = name
            return link, Notice(NoticeKind.CONNECTED, f"connected to {name}{moved}")
        return None

    def _resolve_name(self) -> str | None:
        """The port to open: where the recorded device is now; the requested name while it
        is absent (opening fails and we wait); None if that name now belongs to another
        device (on Linux a freed ttyACM0 is reused by the next board plugged in)."""
        ports = self._safe_list_ports()
        identity = self._identity
        if identity is None or not (identity.serial_number or identity.location):
            return self._requested
        for port in ports:
            if identity.same_device(port):
                return port.device
        if any(port.device == self._requested for port in ports):
            return None
        return self._requested

    def _port_info(self, name: str) -> PortInfo | None:
        return next((port for port in self._safe_list_ports() if port.device == name), None)

    def _safe_list_ports(self) -> list[PortInfo]:
        try:
            return self._list_ports()
        except OSError:
            return []

    def _open(self, name: str) -> SerialLike:
        link = self._open_serial()
        link.port = name
        link.baudrate = self._baudrate
        link.timeout = self._read_timeout
        # Released before open(): active lines would hold the C3 in reset or download mode.
        link.dtr = False
        link.rts = False
        link.open()
        return link


def _open_failure(exc: BaseException) -> str:
    # pyserial wraps the OS error in its message, e.g. "could not open port 'COM6':
    # PermissionError(13, 'Acesso negado.', None, 5)": match the class name, which is the same
    # in every Windows language, not the localized text.
    text = str(exc)
    if isinstance(exc, PermissionError) or "PermissionError" in text:
        return "port busy (is a serial monitor open?)"
    return "not available"


def _close_quietly(link: SerialLike) -> None:
    with contextlib.suppress(serial.SerialException, OSError):
        link.close()


class FileTransport:
    """Reads a saved log for `esp-logcat replay`: the same chunks as SerialTransport, from a file.

    Yields a SOURCE notice first (not CONNECTED: that would turn on the native port's FIFO
    handling, meaningless for a file), then the file's bytes as they become available (read1:
    a pipe from a running program is shown as it arrives, not every 64 KiB).

    Encoding: a byte order mark (UTF-8, UTF-16 LE/BE) is honored and removed, and UTF-16
    without a BOM is recognized by its NUL bytes; UTF-16 is passed on as UTF-8. So a file saved
    with `>` in Windows PowerShell 5.1 (UTF-16) replays like one saved by cmd. Device lines are
    ASCII; non-ASCII text may already have been mangled by the shell that saved it.

    A read error (removed drive, dropped network share) ends the replay with a DISCONNECTED
    notice and sets `failed`, so the command exits non-zero: never a silently short replay.
    """

    CHUNK_SIZE = 64 * 1024
    _SNIFF_BYTES = 64  # enough to recognize UTF-16 without a BOM

    def __init__(self, file: BinaryIO, name: str, *, close: bool = True) -> None:
        self.name = name
        self.failed = False
        self._file = file
        self._close_file = close
        self._closed = False
        self._started = False

    @classmethod
    def open(cls, path: str) -> "FileTransport":
        """Opens `path` ("-" reads stdin). Raises OSError if it cannot be read."""
        if path == "-":
            return cls(sys.stdin.buffer, "stdin", close=False)
        if os.path.isdir(path):  # Windows reports a directory as "Permission denied"
            raise IsADirectoryError(errno.EISDIR, "is a directory", path)
        return cls(open(path, "rb"), path)  # noqa: SIM115 (closed by chunks() or close())

    def close(self) -> None:
        """Stops the replay. Safe from another thread: the reading generator closes the file
        itself (closing it under a running read would raise ValueError there)."""
        self._closed = True
        if not self._started:
            self._release()

    def _release(self) -> None:
        if self._close_file and not self._file.closed:
            self._file.close()

    def _read(self) -> bytes:
        read1 = getattr(self._file, "read1", None)
        return read1(self.CHUNK_SIZE) if read1 is not None else self._file.read(self.CHUNK_SIZE)

    def chunks(self) -> Iterator[bytes | Notice]:
        self._started = True
        try:
            text = f"replaying {self.name}"
            if self.name == "stdin" and _is_terminal(self._file):
                text += " from the terminal (end with Ctrl+Z, Enter; or pipe a file in)"
            yield Notice(NoticeKind.SOURCE, text)
            head = b""  # first bytes, kept until the encoding is known
            decoder: codecs.IncrementalDecoder | None = None
            sniffed = False
            while not self._closed:
                try:
                    chunk = self._read()
                except OSError as exc:
                    self.failed = True
                    reason = exc.strerror or exc
                    yield Notice(NoticeKind.DISCONNECTED, f"{self.name}: read error ({reason})")
                    return
                eof = not chunk
                if not sniffed:
                    head += chunk
                    if not eof and len(head) < self._SNIFF_BYTES:
                        continue
                    sniffed = True
                    decoder, chunk = _detect_encoding(head)
                if decoder is not None:
                    chunk = decoder.decode(chunk, final=eof).encode("utf-8", "replace")
                if chunk:
                    yield chunk
                if eof:
                    return
        finally:
            self._release()


_UTF16_BOMS = {codecs.BOM_UTF16_LE: "utf-16-le", codecs.BOM_UTF16_BE: "utf-16-be"}


def _detect_encoding(head: bytes) -> tuple[codecs.IncrementalDecoder | None, bytes]:
    """Returns the decoder for the file (None: pass bytes through) and `head` without its BOM."""
    if head.startswith(codecs.BOM_UTF8):
        return None, head[len(codecs.BOM_UTF8) :]
    for bom, encoding in _UTF16_BOMS.items():
        if head.startswith(bom):
            return codecs.getincrementaldecoder(encoding)("replace"), head[len(bom) :]
    # UTF-16 without a BOM: ASCII text has a NUL in every other byte. Device logs never contain
    # NUL bytes, so a mostly-NUL half is a reliable sign.
    even, odd = head[0::2], head[1::2]
    if len(head) >= 4:
        if odd.count(0) > len(odd) * 0.4 and even.count(0) < len(even) * 0.1:
            return codecs.getincrementaldecoder("utf-16-le")("replace"), head
        if even.count(0) > len(even) * 0.4 and odd.count(0) < len(odd) * 0.1:
            return codecs.getincrementaldecoder("utf-16-be")("replace"), head
    return None, head


def _is_terminal(file: BinaryIO) -> bool:
    try:
        return file.isatty()
    except (OSError, ValueError):
        return False
