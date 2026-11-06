import random
from pathlib import Path

import pytest

from esp_logcat.parser import (
    CommandReply,
    DumpBegin,
    DumpEnd,
    IdfLine,
    InvalidLine,
    InvalidReason,
    LogLine,
    RawLine,
    parse_line,
)

FIXTURES = Path(__file__).parent / "fixtures"
NOMINAL = b"0042:000137 0000123456 W PRESSURE: reading out of range (4.71 V)\n"


def make_line(
    boot: int = 1,
    seq: int = 1,
    uptime: int = 1,
    level: str = "I",
    tag: str = "APP",
    msg: str = "x",
) -> bytes:
    """Formats a line exactly like the device formatter (PRD §3) for valid inputs."""
    return f"{boot:04d}:{seq:06d} {uptime:010d} {level} {tag}: {msg}\n".encode("ascii")


def assert_invalid(data: bytes, reason: InvalidReason) -> None:
    line = parse_line(data)
    assert isinstance(line, InvalidLine), line
    assert line.reason is reason
    assert line.raw == data


# ------------------------------------------------------------------ valid PRD lines


def test_parses_nominal_line():
    assert parse_line(NOMINAL) == LogLine(
        boot=42,
        seq=137,
        uptime_ms=123456,
        level="W",
        tag="PRESSURE",
        msg="reading out of range (4.71 V)",
        truncated=False,
    )


def test_parses_maximum_field_values():
    data = make_line(9999, 999999, 4294967295, "V", "ABCDEFGHIJKLMNO", "y" * 127)
    assert len(data) == 170
    line = parse_line(data)
    assert isinstance(line, LogLine)
    assert (line.boot, line.seq, line.uptime_ms) == (9999, 999999, 4294967295)
    assert line.tag == "ABCDEFGHIJKLMNO"
    assert len(line.msg) == 127
    assert not line.truncated


def test_minimal_valid_line_is_29_bytes():
    data = b"0001:000001 0000000001 I A: \n"
    assert len(data) == 29
    assert isinstance(parse_line(data), LogLine)


def test_empty_message_and_dash_tag():
    line = parse_line(b"0001:000001 0000000001 I -: \n")
    assert isinstance(line, LogLine)
    assert (line.tag, line.msg) == ("-", "")


def test_message_is_kept_raw():
    data = b"0001:000001 0000000001 I APP:   0042:000001 0000000001 E FAKE: x  \n"
    line = parse_line(data)
    assert isinstance(line, LogLine)
    assert line.msg == "  0042:000001 0000000001 E FAKE: x  "


def test_truncated_is_127_chars_ending_in_dots():
    line = parse_line(make_line(msg="x" * 124 + "..."))
    assert isinstance(line, LogLine)
    assert line.truncated
    assert line.msg.endswith("...")


@pytest.mark.parametrize("msg", ["x" * 127, "wait..."])
def test_not_truncated_without_full_message_and_marker(msg):
    line = parse_line(make_line(msg=msg))
    assert isinstance(line, LogLine)
    assert not line.truncated


def test_every_level_letter():
    for letter in "EWIDV":
        line = parse_line(make_line(level=letter))
        assert isinstance(line, LogLine)
        assert line.level == letter


# ------------------------------------------------------------------ invalid PRD lines


def test_line_without_newline_is_torn():
    assert_invalid(NOMINAL[:-1], InvalidReason.TORN)


def test_every_prefix_of_a_line_is_torn():
    full = make_line(9999, 999999, 4294967295, "V", "ABCDEFGHIJKLMNO", "y" * 127)
    for line in (NOMINAL, full):
        for cut in range(1, len(line)):
            assert_invalid(line[:cut], InvalidReason.TORN)
            assert_invalid(line[:cut] + b"\x7f\n", InvalidReason.TORN)


def test_del_inside_a_line_is_garbage():
    assert_invalid(NOMINAL[:40] + b"\x7f" + NOMINAL[41:], InvalidReason.GARBAGE)


@pytest.mark.parametrize("byte", [b"\r", b"\x00", b"\x01", b"\x1b", b"\x80", b"\xff", b"\n"])
def test_non_printable_byte_is_garbage(byte):
    assert_invalid(NOMINAL[:40] + byte + NOMINAL[41:], InvalidReason.GARBAGE)


def test_crlf_is_garbage_for_the_strict_parser():
    # Serial transports normalize CRLF before parsing (the ESP-IDF console sends CRLF);
    # the parser itself mirrors the device contract.
    assert_invalid(NOMINAL[:-1] + b"\r\n", InvalidReason.GARBAGE)


def test_line_over_170_bytes_is_overlong():
    assert_invalid(make_line(tag="ABCDEFGHIJKLMNO", msg="y" * 128), InvalidReason.OVERLONG)


def test_message_over_127_chars_is_overlong():
    assert_invalid(make_line(tag="A", msg="m" * 128), InvalidReason.OVERLONG)


def test_line_shorter_than_29_bytes_is_bad_header():
    assert_invalid(b"0001:000001 0000000001 I : \n", InvalidReason.BAD_HEADER)


@pytest.mark.parametrize("index", [i for i in range(25) if i != 23])
def test_each_broken_header_position_is_bad_header(index):
    wrong = b"x" if NOMINAL[index : index + 1].isdigit() else b"_"
    assert_invalid(NOMINAL[:index] + wrong + NOMINAL[index + 1 :], InvalidReason.BAD_HEADER)


@pytest.mark.parametrize(
    "data",
    [
        b"0042:-00137 0000123456 W A: x\n",
        b"0042:000137 +000123456 W A: x\n",
        b"0042:000137 0x00123456 W A: x\n",
        b"0042:000137  000123456 W A: x\n",
    ],
)
def test_sign_space_and_hex_in_numbers_are_bad_header(data):
    assert_invalid(data, InvalidReason.BAD_HEADER)


@pytest.mark.parametrize("uptime", [b"4294967296", b"9999999999"])
def test_uptime_above_32_bits_is_bad_header(uptime):
    assert_invalid(b"0001:000001 " + uptime + b" I A: x\n", InvalidReason.BAD_HEADER)


@pytest.mark.parametrize("letter", [b"?", b"e", b"X", b"N"])
def test_invalid_level_is_bad_level(letter):
    assert_invalid(NOMINAL[:23] + letter + NOMINAL[24:], InvalidReason.BAD_LEVEL)


@pytest.mark.parametrize(
    "data",
    [
        b"0001:000001 0000000001 I : message\n",
        b"0001:000001 0000000001 I ABCDEFGHIJKLMNOP: x\n",
        b"0001:000001 0000000001 I A B: x\n",
        b"0001:000001 0000000001 I APP:x\n",
        b"0001:000001 0000000001 I APP no colon\n",
        b"0001:000001 0000000001 I APP:\n",
    ],
)
def test_bad_tags_are_bad_header(data):
    assert_invalid(data, InvalidReason.BAD_HEADER)


def test_lenient_mode_accepts_missing_space_after_tag():
    # Fixtures edited by hand may lose the trailing space of an empty message.
    data = b"0001:000001 0000000001 I APP:\n"
    line = parse_line(data, lenient=True)
    assert isinstance(line, LogLine)
    assert (line.tag, line.msg) == ("APP", "")


# ------------------------------------------------------------------ other line types


def test_esp_idf_native_line():
    assert parse_line(b"I (253) APP: formatted 49 bytes, LogRecord=160 bytes\n") == IdfLine(
        level="I", timestamp_ms=253, tag="APP", msg="formatted 49 bytes, LogRecord=160 bytes"
    )


def test_esp_idf_line_tag_with_dots_and_colon_in_message():
    line = parse_line(b"I (41) boot.esp32c3: SPI Flash Size : 4MB\n")
    assert line == IdfLine(
        level="I", timestamp_ms=41, tag="boot.esp32c3", msg="SPI Flash Size : 4MB"
    )


def test_esp_idf_line_with_ansi_colors():
    line = parse_line(b"\x1b[0;31mE (1234) wifi: connection lost\x1b[0m\n")
    assert line == IdfLine(level="E", timestamp_ms=1234, tag="wifi", msg="connection lost")


def test_command_reply():
    assert parse_line(b"#> boot=42 reason=PANIC fw=0.3.0\n") == CommandReply(
        text="boot=42 reason=PANIC fw=0.3.0"
    )


def test_dump_markers():
    assert parse_line(b"#BEGIN log_003.txt 4096\n") == DumpBegin(filename="log_003.txt", size=4096)
    assert parse_line(b"#END 1a2b3c4d\n") == DumpEnd(crc32=0x1A2B3C4D)


@pytest.mark.parametrize(
    "data",
    [
        b"ESP-ROM:esp32c3-api1-20210207\n",
        b"rst:0x15 (USB_UART_CHIP_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)\n",
        b"I (101) esp_image: segment 1: paddr=0001803c vaddr=3fc89a00 size",  # stale fragment
        b"#BEGIN missing-size\n",
        b"\n",
        b"",
    ],
)
def test_anything_else_is_raw(data):
    assert isinstance(parse_line(data), RawLine)


def test_raw_text_is_decoded_without_the_newline():
    assert parse_line(b"hello \xff world\n") == RawLine(text="hello � world")


# ------------------------------------------------------------------ real device capture


def split_lines(data: bytes) -> list[bytes]:
    parts = data.split(b"\n")
    lines = [part + b"\n" for part in parts[:-1]]
    if parts[-1]:
        lines.append(parts[-1])  # unterminated tail
    return lines


def test_real_boot_capture_from_the_board():
    data = (FIXTURES / "boot_esp32c3_rev4.log").read_bytes()
    assert b"\r\n" in data  # the ESP-IDF console really sends CRLF
    parsed = [parse_line(line.replace(b"\r\n", b"\n")) for line in split_lines(data)]

    log_lines = [line for line in parsed if isinstance(line, LogLine)]
    assert log_lines == [
        LogLine(
            boot=0,
            seq=0,
            uptime_ms=0,
            level="I",
            tag="APP",
            msg="esp32-logcat hello",
            truncated=False,
        )
    ]
    idf = [line for line in parsed if isinstance(line, IdfLine)]
    assert any(line.tag == "boot" and "2nd stage bootloader" in line.msg for line in idf)
    assert any(line.tag == "APP" and line.msg.startswith("parsed back: ok") for line in idf)
    raw = [line for line in parsed if isinstance(line, RawLine)]
    assert any(line.text.startswith("ESP-ROM:") for line in raw)
    assert not [line for line in parsed if isinstance(line, InvalidLine)]


# ------------------------------------------------------------------ properties


def test_random_valid_lines_parse_to_their_fields():
    rng = random.Random(2026)
    printable = [chr(c) for c in range(0x20, 0x7F)]
    tag_chars = [c for c in printable if c not in " :"]
    for _ in range(3000):
        boot, seq, uptime = rng.randrange(10000), rng.randrange(10**6), rng.randrange(2**32)
        level = rng.choice("EWIDV")
        tag = "".join(rng.choices(tag_chars, k=rng.randint(1, 15)))
        msg = "".join(rng.choices(printable, k=rng.randint(0, 127)))
        line = parse_line(make_line(boot, seq, uptime, level, tag, msg))
        assert line == LogLine(
            boot=boot,
            seq=seq,
            uptime_ms=uptime,
            level=level,
            tag=tag,
            msg=msg,
            truncated=len(msg) == 127 and msg.endswith("..."),
        )


def test_corrupted_lines_never_raise():
    rng = random.Random(7)
    for _ in range(5000):
        data = bytearray(make_line(msg="".join(rng.choices("abc xyz", k=rng.randint(0, 60)))))
        for _ in range(rng.randint(1, 3)):
            data[rng.randrange(len(data))] = rng.randrange(256)
        line = parse_line(bytes(data))  # must classify, never raise
        assert line is not None
