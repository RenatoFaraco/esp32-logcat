import codecs
import errno
import io
import random
import sys
from collections.abc import Iterator
from pathlib import Path

import pytest
from serial import SerialException

from esp_logcat.events import Notice, NoticeKind
from esp_logcat.transport import (
    MAX_PENDING,
    FileTransport,
    LineSplitter,
    PortInfo,
    PortSelectionError,
    SerialTransport,
    find_port,
    pulse_reset,
)

FIXTURES = Path(__file__).parent / "fixtures"
NATIVE = PortInfo("COM6", 0x303A, 0x1001, "80:53:E0:0A:5D:AC", None, "Dispositivo Serial USB")
BRIDGE = PortInfo("COM5", 0x1A86, 0x7522, None, "1-3.1", "USB-SERIAL CH340K")


# ---------------------------------------------------------------- LineSplitter


def split_all(chunks: list[bytes], max_pending: int = MAX_PENDING) -> list[bytes]:
    splitter = LineSplitter(max_pending)
    lines = [line for chunk in chunks for line in splitter.feed(chunk)]
    return lines + splitter.flush()


def test_crlf_becomes_lf():
    assert split_all([b"I (1) T: a\r\nI (2) T: b\r\n"]) == [b"I (1) T: a\n", b"I (2) T: b\n"]


def test_lone_cr_is_kept_for_the_parser():
    assert split_all([b"a\rb\n", b"c\r\r\n"]) == [b"a\rb\n", b"c\r\n"]


def test_line_split_across_chunks_including_cr_lf_boundary():
    assert split_all([b"0042:00", b"0137 x\r", b"\n", b"next\n"]) == [b"0042:000137 x\n", b"next\n"]


def test_unterminated_tail_comes_out_on_flush():
    splitter = LineSplitter()
    assert splitter.feed(b"done\npartial") == [b"done\n"]
    assert splitter.flush() == [b"partial"]
    assert splitter.flush() == []


def test_overlong_line_is_cut_and_resynchronized():
    lines = split_all([b"x" * 30 + b"\nok\n"], max_pending=10)
    assert lines == [b"x" * 10, b"ok\n"]


def test_overlong_across_chunks_discards_until_newline():
    splitter = LineSplitter(max_pending=10)
    assert splitter.feed(b"y" * 8) == []
    assert splitter.feed(b"y" * 8) == [b"y" * 10]
    assert splitter.feed(b"zzzz") == []  # still discarding
    assert splitter.feed(b"zz\nok\n") == [b"ok\n"]
    assert splitter.overflows == 1
    assert splitter.flush() == []


def test_line_of_exactly_max_pending_is_kept_even_with_crlf():
    body = b"a" * 10
    for chunks in ([body + b"\r\n"], [body + b"\r", b"\n"], [body, b"\r\n"]):
        splitter = LineSplitter(max_pending=10)
        lines = [line for chunk in chunks for line in splitter.feed(chunk)]
        assert lines == [body + b"\n"], chunks
        assert splitter.overflows == 0


def test_default_limit_keeps_long_esp_idf_lines():
    line = b"I (1) T: " + b"m" * 400 + b"\n"
    assert split_all([line]) == [line]


@pytest.mark.parametrize("seed", range(20))
def test_chunking_never_changes_the_result(seed):
    data = (FIXTURES / "boot_esp32c3_rev4.log").read_bytes()
    data += b"\r\n" + b"n" * 50 + b"\r\nend\r" + b"\n" + b"q" * 40 + b"\r\n"
    expected = split_all([data], max_pending=32)
    rng = random.Random(seed)
    chunks, pos = [], 0
    while pos < len(data):
        size = rng.randint(1, 64)
        chunks.append(data[pos : pos + size])
        pos += size
    assert split_all(chunks, max_pending=32) == expected


def test_board_capture_lines_match_splitlines():
    data = (FIXTURES / "boot_esp32c3_rev4.log").read_bytes()
    expected = data.replace(b"\r\n", b"\n").splitlines(keepends=True)
    assert split_all([data]) == expected


# ---------------------------------------------------------------- port selection


def test_find_port_prefers_the_native_usb_port():
    assert find_port([BRIDGE, NATIVE]) == "COM6"


def test_find_port_single_port():
    assert find_port([BRIDGE]) == "COM5"


@pytest.mark.parametrize(
    "ports",
    [
        [],
        [BRIDGE, PortInfo("COM3", description="Bluetooth")],
        [NATIVE, PortInfo("COM9", 0x303A, 0x1001, "AA:BB")],
    ],
)
def test_find_port_refuses_to_guess(ports):
    with pytest.raises(PortSelectionError, match="--port"):
        find_port(ports)


def test_same_device_by_serial_number_or_location():
    assert NATIVE.same_device(PortInfo("COM7", 0x303A, 0x1001, "80:53:E0:0A:5D:AC"))
    assert not NATIVE.same_device(PortInfo("COM7", 0x303A, 0x1001, "11:22:33:44:55:66"))
    assert BRIDGE.same_device(PortInfo("COM8", 0x1A86, 0x7522, None, "1-3.1"))
    assert not BRIDGE.same_device(PortInfo("COM8", 0x1A86, 0x7522, None, "1-4"))
    assert not PortInfo("COM1").same_device(PortInfo("COM2"))


# ---------------------------------------------------------------- SerialTransport (fake bus)


class EndOfScript(Exception):
    """Raised by the fake when a test has nothing more to deliver (not caught by transport)."""


class FakeBus:
    """Ports that exist, are busy, and what each read returns (bytes or an exception)."""

    def __init__(self, ports: list[PortInfo]) -> None:
        self.ports = list(ports)
        self.busy: set[str] = set()
        self.reads: dict[str, list[bytes | Exception]] = {}
        self.links: list[FakeSerial] = []
        self.sleeps: list[float] = []
        self.on_sleep = None
        self.in_waiting_errors: dict[str, Exception] = {}
        self.written: list[tuple[str | None, bytes]] = []
        self.write_errors: dict[str, Exception] = {}

    def serial(self) -> "FakeSerial":
        link = FakeSerial(self)
        self.links.append(link)
        return link

    def list_ports(self) -> list[PortInfo]:
        return list(self.ports)

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        if len(self.sleeps) > 50:  # a transport retrying forever: end the test, don't hang
            raise EndOfScript
        if self.on_sleep is not None:
            self.on_sleep(len(self.sleeps))


class FakeSerial:
    def __init__(self, bus: FakeBus) -> None:
        self.bus = bus
        self.port = None
        self.baudrate = 9600
        self.timeout = None
        self._dtr = True  # pyserial's default: lines active
        self._rts = True
        self.ops: list[tuple[str, object]] = []
        self.closed = False

    @property
    def dtr(self) -> bool:
        return self._dtr

    @dtr.setter
    def dtr(self, value: bool) -> None:
        self._dtr = value
        self.ops.append(("dtr", value))

    @property
    def rts(self) -> bool:
        return self._rts

    @rts.setter
    def rts(self, value: bool) -> None:
        self._rts = value
        self.ops.append(("rts", value))

    @property
    def in_waiting(self) -> int:
        # On Windows a removed device usually fails first here (ClearCommError).
        error = self.bus.in_waiting_errors.pop(self.port, None)
        if error is not None:
            raise error
        return 0

    def open(self) -> None:
        self.ops.append(("open", (self._dtr, self._rts)))
        if self.port in self.bus.busy:
            raise SerialException(
                f"could not open port '{self.port}': PermissionError(13, 'Acesso negado.', None, 5)"
            )
        if not any(port.device == self.port for port in self.bus.ports):
            raise SerialException(f"could not open port '{self.port}': FileNotFoundError(2)")

    def read(self, size: int) -> bytes:
        script = self.bus.reads.get(self.port, [])
        if not script:
            raise EndOfScript
        item = script.pop(0)
        if isinstance(item, Exception):
            raise item
        return item

    def write(self, data: bytes) -> int:
        error = self.bus.write_errors.pop(self.port, None)
        if error is not None:
            raise error
        self.bus.written.append((self.port, data))
        return len(data)

    def close(self) -> None:
        self.closed = True


def make_transport(bus: FakeBus, port: str = "COM6", **kwargs: object) -> SerialTransport:
    return SerialTransport(
        port,
        open_serial=bus.serial,
        list_ports=bus.list_ports,
        sleep=bus.sleep,
        **kwargs,  # type: ignore[arg-type]
    )


def collect(chunks: Iterator[bytes | Notice]) -> list[bytes | Notice]:
    items: list[bytes | Notice] = []
    try:
        for item in chunks:
            items.append(item)
    except EndOfScript:
        pass
    return items


def kinds(items: list[bytes | Notice]) -> list[object]:
    return [item.kind if isinstance(item, Notice) else item for item in items]


def test_opens_with_dtr_and_rts_released_before_open():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [b"hello\n"]
    items = collect(make_transport(bus).chunks())
    assert kinds(items) == [NoticeKind.CONNECTED, b"hello\n"]
    link = bus.links[0]
    assert ("open", (False, False)) in link.ops
    assert link.port == "COM6" and link.baudrate == 115200 and link.timeout == pytest.approx(0.1)


def test_empty_reads_are_skipped():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [b"", b"a", b"", b"b"]
    assert kinds(collect(make_transport(bus).chunks()))[1:] == [b"a", b"b"]


def test_reset_pulse_rewrites_dtr_after_each_rts_change():
    bus = FakeBus([NATIVE])
    link = bus.serial()
    pulse_reset(link, bus.sleep)
    assert link.ops == [
        ("dtr", False),
        ("rts", True),
        ("dtr", False),
        ("rts", False),
        ("dtr", False),
    ]
    assert bus.sleeps == [pytest.approx(0.2)]


def test_reset_only_on_the_first_connection():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [SerialException("gone")]
    collect(make_transport(bus, reset=True).chunks())  # reset, read fails, reconnect, end
    rts_ops = [[op for op in link.ops if op[0] == "rts"] for link in bus.links]
    assert ("rts", True) in rts_ops[0]
    assert all(("rts", True) not in ops for ops in rts_ops[1:])


def test_lost_connection_reopens_the_same_port():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [b"one\n", SerialException("ClearCommError failed"), b"two\n"]
    items = collect(make_transport(bus).chunks())
    assert kinds(items) == [
        NoticeKind.CONNECTED,
        b"one\n",
        NoticeKind.DISCONNECTED,
        NoticeKind.CONNECTED,
        b"two\n",
    ]
    assert bus.links[0].closed


def test_waits_for_an_absent_port_and_reports_once():
    bus = FakeBus([])
    bus.reads["COM6"] = [b"late\n"]

    def appear(count: int) -> None:
        if count == 3:
            bus.ports.append(NATIVE)

    bus.on_sleep = appear
    items = collect(make_transport(bus).chunks())
    assert kinds(items) == [NoticeKind.WAITING, NoticeKind.CONNECTED, b"late\n"]
    assert "not available" in items[0].text
    assert bus.sleeps == [0.5, 0.5, 0.5]


def test_busy_port_says_so():
    bus = FakeBus([NATIVE])
    bus.busy.add("COM6")
    bus.reads["COM6"] = [b"x"]
    bus.on_sleep = lambda count: bus.busy.clear()
    items = collect(make_transport(bus).chunks())
    assert items[0].kind is NoticeKind.WAITING
    assert "busy" in items[0].text


def test_native_port_found_again_under_a_new_number():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [SerialException("device removed")]
    bus.reads["COM7"] = [b"back\n"]
    transport = make_transport(bus)
    chunks = transport.chunks()
    assert next(chunks).kind is NoticeKind.CONNECTED  # identity recorded here
    bus.ports = []  # unplugged...
    renumbered = PortInfo("COM7", 0x303A, 0x1001, "80:53:E0:0A:5D:AC")
    bus.on_sleep = lambda count: setattr(bus, "ports", [renumbered])  # ...back as COM7
    items = collect(chunks)
    assert kinds(items) == [NoticeKind.DISCONNECTED, NoticeKind.CONNECTED, b"back\n"]
    assert items[1].text == "connected to COM7 (was COM6)"
    assert transport.port == "COM7"
    assert bus.sleeps == [0.5]  # one pause after the drop, never a tight reconnect loop


def test_bridge_found_again_by_location():
    bus = FakeBus([BRIDGE])
    bus.reads["COM5"] = [SerialException("gone")]
    bus.reads["COM8"] = [b"uart\n"]
    transport = make_transport(bus, port="COM5")
    chunks = transport.chunks()
    assert next(chunks).kind is NoticeKind.CONNECTED
    bus.ports = [PortInfo("COM8", 0x1A86, 0x7522, None, "1-3.1")]
    items = collect(chunks)
    assert kinds(items)[-1] == b"uart\n"
    assert transport.port == "COM8"


def test_other_device_on_a_new_number_is_not_taken():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [SerialException("gone")]
    bus.reads["COM9"] = [b"someone else\n"]
    transport = make_transport(bus)
    chunks = transport.chunks()
    next(chunks)
    bus.ports = [PortInfo("COM9", 0x303A, 0x1001, "11:22:33:44:55:66")]
    bus.on_sleep = lambda count: transport.close() if count >= 3 else None
    items = collect(chunks)
    assert b"someone else\n" not in items
    assert all(link.port != "COM9" for link in bus.links)


def test_close_stops_the_stream():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [b"a", b"b", b"c"]
    transport = make_transport(bus)
    chunks = transport.chunks()
    assert next(chunks).kind is NoticeKind.CONNECTED
    assert next(chunks) == b"a"
    transport.close()
    assert list(chunks) == []
    assert bus.links[0].closed


def test_link_closed_when_the_consumer_stops():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [b"a", b"b"]
    chunks = make_transport(bus).chunks()
    next(chunks)
    next(chunks)
    chunks.close()
    assert bus.links[0].closed


def test_drop_seen_first_by_in_waiting():
    bus = FakeBus([NATIVE])
    bus.in_waiting_errors["COM6"] = SerialException("ClearCommError failed")
    bus.reads["COM6"] = [b"after\n"]
    items = collect(make_transport(bus).chunks())
    assert kinds(items) == [
        NoticeKind.CONNECTED,
        NoticeKind.DISCONNECTED,
        NoticeKind.CONNECTED,
        b"after\n",
    ]


def test_plain_oserror_on_read_is_a_drop():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [OSError(22, "Invalid argument"), b"x"]
    assert kinds(collect(make_transport(bus).chunks()))[:2] == [
        NoticeKind.CONNECTED,
        NoticeKind.DISCONNECTED,
    ]


def test_name_reused_by_another_device_is_not_opened():
    # Linux: board A leaves ttyACM0, board B arrives and gets ttyACM0.
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [SerialException("gone")]
    transport = make_transport(bus)
    chunks = transport.chunks()
    next(chunks)
    bus.ports = [PortInfo("COM6", 0x303A, 0x1001, "11:22:33:44:55:66")]
    bus.on_sleep = lambda count: transport.close() if count >= 3 else None
    items = collect(chunks)
    assert kinds(items) == [NoticeKind.DISCONNECTED, NoticeKind.WAITING]
    assert "another device" in items[1].text
    assert len(bus.links) == 1  # never reopened


def test_ctrl_c_during_the_reset_pulse_releases_rts():
    bus = FakeBus([NATIVE])
    link = bus.serial()

    def interrupted(seconds: float) -> None:
        raise KeyboardInterrupt

    with pytest.raises(KeyboardInterrupt):
        pulse_reset(link, interrupted)
    assert link.rts is False
    assert link.ops[-1] == ("dtr", False)  # the release reached the device (usbser.sys)


def test_ctrl_c_during_the_reset_closes_the_port():
    bus = FakeBus([NATIVE])

    def sleep(seconds: float) -> None:
        if seconds == pytest.approx(0.2):
            raise KeyboardInterrupt

    transport = SerialTransport(
        "COM6", reset=True, open_serial=bus.serial, list_ports=bus.list_ports, sleep=sleep
    )
    with pytest.raises(KeyboardInterrupt):
        list(transport.chunks())
    assert bus.links[0].closed


def test_consumer_stopping_at_connected_closes_the_port():
    bus = FakeBus([NATIVE])
    chunks = make_transport(bus).chunks()
    assert next(chunks).kind is NoticeKind.CONNECTED
    chunks.close()
    assert bus.links[0].closed


# ---------------------------------------------------------------- FileTransport (replay)


def replay_chunks(path) -> list[bytes | Notice]:
    return list(FileTransport.open(str(path)).chunks())


def test_file_transport_announces_the_source_then_yields_the_bytes(tmp_path):
    path = tmp_path / "session.log"
    data = (FIXTURES / "two_boots_esp32c3.log").read_bytes()
    path.write_bytes(data)
    items = replay_chunks(path)
    assert items[0] == Notice(NoticeKind.SOURCE, f"replaying {path}")
    assert b"".join(items[1:]) == data


def test_file_transport_reads_large_files_in_chunks(tmp_path):
    path = tmp_path / "big.log"
    path.write_bytes(b"x" * (FileTransport.CHUNK_SIZE * 2 + 5))
    sizes = [len(item) for item in replay_chunks(path)[1:]]
    assert sizes == [FileTransport.CHUNK_SIZE, FileTransport.CHUNK_SIZE, 5]


@pytest.mark.parametrize(
    "encode",
    [
        lambda text: codecs.BOM_UTF16_LE + text.encode("utf-16-le"),  # PowerShell 5.1 ">"
        lambda text: codecs.BOM_UTF16_BE + text.encode("utf-16-be"),
        lambda text: codecs.BOM_UTF8 + text.encode("utf-8"),
        lambda text: text.encode("utf-8"),
    ],
)
def test_file_transport_reads_any_saved_encoding_as_the_same_bytes(tmp_path, encode):
    text = "0000:000001 0000000010 I APP: hello\r\n" * 3000  # spans several chunks
    path = tmp_path / "saved.log"
    path.write_bytes(encode(text))
    assert b"".join(replay_chunks(path)[1:]) == text.encode("ascii")


def test_file_transport_empty_file(tmp_path):
    path = tmp_path / "empty.log"
    path.write_bytes(b"")
    assert kinds(replay_chunks(path)) == [NoticeKind.SOURCE]


def test_file_transport_reads_stdin(monkeypatch):
    stdin = io.TextIOWrapper(io.BytesIO(b"from a pipe\n"))
    monkeypatch.setattr(sys, "stdin", stdin)
    transport = FileTransport.open("-")
    items = list(transport.chunks())
    assert items == [Notice(NoticeKind.SOURCE, "replaying stdin"), b"from a pipe\n"]
    assert not stdin.closed  # never closes the process's stdin


def test_file_transport_closes_the_file(tmp_path):
    path = tmp_path / "a.log"
    path.write_bytes(b"a\nb\n")
    transport = FileTransport.open(str(path))
    chunks = transport.chunks()
    next(chunks)
    chunks.close()  # consumer stops early (Ctrl+C, closed pipe)
    assert transport._file.closed


def test_file_transport_missing_file_raises(tmp_path):
    with pytest.raises(OSError):
        FileTransport.open(str(tmp_path / "nope.log"))


def test_file_transport_truncated_utf16_ends_with_a_replacement_char(tmp_path):
    # A UTF-16 file cut in the middle of a character: the last odd byte is not lost silently.
    path = tmp_path / "cut.log"
    path.write_bytes(codecs.BOM_UTF16_LE + "ok\n".encode("utf-16-le") + b"A")
    assert b"".join(replay_chunks(path)[1:]) == b"ok\n" + "\ufffd".encode()


class Dribble(io.RawIOBase):
    """A binary stream that hands out `size` bytes per read, then optionally fails."""

    def __init__(self, data: bytes, size: int, fail_after: int | None = None) -> None:
        self._data = data
        self._size = size
        self._reads = 0
        self._fail_after = fail_after

    def readable(self) -> bool:
        return True

    def read(self, n: int = -1) -> bytes:
        self._reads += 1
        if self._fail_after is not None and self._reads > self._fail_after:
            raise OSError(errno.EINVAL, "Invalid argument")  # what Windows reports for a lost drive
        out, self._data = self._data[: self._size], self._data[self._size :]
        return out


def dribble_chunks(data: bytes, size: int, **kwargs: int) -> tuple[list, FileTransport]:
    transport = FileTransport(Dribble(data, size, **kwargs), "dribble.log")  # type: ignore[arg-type]
    return list(transport.chunks()), transport


def test_utf16_split_across_reads_including_a_surrogate_pair():
    text = "0000:000001 0000000010 I APP: emoji " + chr(0x1F600) + " end\n"
    data = codecs.BOM_UTF16_LE + text.encode("utf-16-le")
    for size in (1, 3, 5, 7):  # odd sizes split code units and the surrogate pair
        items, _ = dribble_chunks(data, size)
        assert b"".join(items[1:]) == text.encode("utf-8"), size


def test_bom_arriving_one_byte_at_a_time_is_still_removed():
    for bom in (codecs.BOM_UTF8, codecs.BOM_UTF16_LE):
        encoding = "utf-8" if bom == codecs.BOM_UTF8 else "utf-16-le"
        items, _ = dribble_chunks(bom + "ok\n".encode(encoding), 1)
        assert b"".join(items[1:]) == b"ok\n"


@pytest.mark.parametrize("encoding", ["utf-16-le", "utf-16-be"])
def test_utf16_without_bom_is_recognized(tmp_path, encoding):
    text = "0000:000001 0000000010 I APP: no bom here\r\n" * 4
    path = tmp_path / "nobom.log"
    path.write_bytes(text.encode(encoding))
    assert b"".join(replay_chunks(path)[1:]) == text.encode("ascii")


def test_plain_ascii_is_not_mistaken_for_utf16(tmp_path):
    path = tmp_path / "a.log"
    path.write_bytes(b"ab\ncd\n")  # short and NUL-free
    assert b"".join(replay_chunks(path)[1:]) == b"ab\ncd\n"


def test_read_error_ends_with_a_notice_and_marks_the_transport_failed():
    items, transport = dribble_chunks(b"0000:000001 0000000010 I APP: a\n" * 10, 100, fail_after=1)
    assert items[-1].kind is NoticeKind.DISCONNECTED
    assert "read error" in items[-1].text
    assert transport.failed


def test_close_before_reading_releases_the_file(tmp_path):
    path = tmp_path / "a.log"
    path.write_bytes(b"x\n")
    transport = FileTransport.open(str(path))
    transport.close()
    assert transport._file.closed


def test_close_from_outside_stops_the_replay(tmp_path):
    path = tmp_path / "a.log"
    path.write_bytes(b"x" * (FileTransport.CHUNK_SIZE * 3))
    transport = FileTransport.open(str(path))
    chunks = transport.chunks()
    next(chunks)  # SOURCE notice
    next(chunks)  # first chunk
    transport.close()  # e.g. the TUI's stop button, between two reads
    assert list(chunks) == []
    assert transport._file.closed


def test_directory_is_reported_as_such(tmp_path):
    with pytest.raises(IsADirectoryError):
        FileTransport.open(str(tmp_path))


def test_stdin_from_a_terminal_says_how_to_end(monkeypatch):
    class Terminal(io.BytesIO):
        def isatty(self) -> bool:
            return True

    stdin = io.TextIOWrapper(Terminal(b""))
    monkeypatch.setattr(sys, "stdin", stdin)
    notice = next(FileTransport.open("-").chunks())
    assert "from the terminal" in notice.text and "Ctrl+Z" in notice.text


# ------------------------------------------------------------------ write (the shell, #30)


def test_write_before_connecting_is_refused():
    bus = FakeBus([NATIVE])
    transport = make_transport(bus)
    assert not transport.connected
    assert transport.write(b"1 info\n") is False
    assert bus.written == []


def test_write_goes_to_the_open_port_and_stops_when_it_closes():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [b"hello\n"]
    transport = make_transport(bus)
    chunks = transport.chunks()
    assert next(chunks).kind is NoticeKind.CONNECTED
    assert transport.connected
    assert transport.write(b"1 info\n") is True
    assert bus.written == [("COM6", b"1 info\n")]
    collect(chunks)  # the script ends: the port is closed
    assert not transport.connected
    assert transport.write(b"2 info\n") is False


def test_a_failing_write_is_reported_not_raised():
    bus = FakeBus([NATIVE])
    bus.reads["COM6"] = [b"hello\n"]
    bus.write_errors["COM6"] = SerialException("write failed")
    transport = make_transport(bus)
    chunks = transport.chunks()
    next(chunks)
    assert transport.write(b"1 info\n") is False
    chunks.close()
