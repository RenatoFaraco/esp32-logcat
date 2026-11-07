import io
from datetime import datetime
from pathlib import Path

import pytest
from rich.console import Console
from rich.text import Span, Text

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
from esp_logcat.render import (
    DUMP_STYLE,
    ESCAPE_STYLE,
    HEADER_STYLE,
    INVALID_LABEL_STYLE,
    LEVEL_STYLES,
    MARKER_STYLE,
    PC_TIME_STYLE,
    RAW_STYLE,
    REPLY_STYLE,
    render,
)

FIXTURES = Path(__file__).parent / "fixtures"


def fixture_lines(name: str) -> list[bytes]:
    data = (FIXTURES / name).read_bytes().replace(b"\r\n", b"\n")
    return data.splitlines(keepends=True)


def log_line(level: str = "I", tag: str = "APP", msg: str = "hello", **kwargs: object) -> LogLine:
    fields = {"boot": 42, "seq": 137, "uptime_ms": 123456, "truncated": False} | kwargs
    return LogLine(level=level, tag=tag, msg=msg, **fields)  # type: ignore[arg-type]


def styles_of(text: Text, fragment: str) -> set[str]:
    """Styles of every span covering the first occurrence of `fragment`."""
    start = text.plain.index(fragment)
    end = start + len(fragment)
    return {str(span.style) for span in text.spans if span.start <= start and span.end >= end}


def printed(text: Text, *, terminal: bool) -> str:
    out = io.StringIO()
    # Leave color_system on "auto" off a terminal: an explicit one forces colors even there.
    colors = {"color_system": "standard"} if terminal else {}
    console = Console(file=out, force_terminal=terminal, width=500, **colors)
    console.print(text)
    return out.getvalue()


# ---------------------------------------------------------------- layout: same text as the line


def test_log_line_reprints_the_device_line():
    line = b"0042:000137 0000123456 W PRESSURE: reading out of range (4.71 V)\n"
    assert render(parse_line(line)).plain == line.decode().rstrip("\n")


@pytest.mark.parametrize(
    "line", [line for line in fixture_lines("boot_esp32c3_rev4.log") if line.endswith(b"\n")]
)
def test_board_capture_round_trips(line):
    parsed = parse_line(line)
    assert render(parsed).plain == line.decode("utf-8").rstrip("\n")


def test_board_capture_covers_the_three_kinds():
    kinds = {type(parse_line(line)) for line in fixture_lines("boot_esp32c3_rev4.log")}
    assert {LogLine, IdfLine, RawLine} <= kinds


def test_truncated_message_keeps_its_marker():
    msg = "a" * 124 + "..."
    text = render(log_line(msg=msg, truncated=True))
    assert text.plain.endswith(msg)
    assert MARKER_STYLE in styles_of(text, "...")
    assert MARKER_STYLE not in styles_of(text, "a" * 124)


def test_message_that_only_looks_truncated_is_plain():
    text = render(log_line(msg="wait...", truncated=False))
    assert MARKER_STYLE not in styles_of(text, "...")


def test_pc_time_prefix():
    text = render(log_line(), pc_time=datetime(2026, 11, 7, 9, 5, 3, 42_999))
    assert text.plain.startswith("09:05:03.042 0042:000137 ")
    assert PC_TIME_STYLE in styles_of(text, "09:05:03.042")


def test_no_prefix_without_pc_time():
    assert render(log_line()).plain.startswith("0042:000137 0000123456 I APP: ")


# ---------------------------------------------------------------- styles


@pytest.mark.parametrize("level", list("EWIDV"))
def test_log_line_level_color(level):
    text = render(log_line(level=level, tag="PRESSURE", msg="value 3"))
    color = LEVEL_STYLES[level]
    assert f"bold {color}" in styles_of(text, "PRESSURE")
    assert f"bold {color}" in styles_of(text, level)  # the header has digits only
    assert color in styles_of(text, "value 3")
    assert HEADER_STYLE in styles_of(text, "0042:000137 0000123456")


@pytest.mark.parametrize("level", list("EWIDV"))
def test_idf_line_level_color_without_bold_or_dim(level):
    text = render(IdfLine(level=level, timestamp_ms=253, tag="boot", msg="ready"))
    assert text.plain == f"{level} (253) boot: ready"
    assert styles_of(text, "ready") == {LEVEL_STYLES[level]}
    assert styles_of(text, "boot") == {LEVEL_STYLES[level]}


def test_unknown_level_does_not_crash():
    text = render(log_line(level="X", msg="m"))
    assert text.plain == "0042:000137 0000123456 X APP: m"
    text = render(IdfLine(level="\x1b", timestamp_ms=1, tag="T", msg="m"))
    assert text.plain == "\\x1b (1) T: m"


def test_levels_have_distinct_colors():
    assert set(LEVEL_STYLES) == set("EWIDV")
    assert len(set(LEVEL_STYLES.values())) == 5
    assert LEVEL_STYLES["E"] == "red"
    assert LEVEL_STYLES["W"] == "yellow"
    # Readability on common dark schemes (contrast notes in render.py).
    assert LEVEL_STYLES["D"] != "blue"
    assert "bright_black" not in LEVEL_STYLES["V"]


def test_raw_line_is_dimmed():
    text = render(RawLine(text="ESP-ROM:esp32c3-api1-20210207"))
    assert text.plain == "ESP-ROM:esp32c3-api1-20210207"
    assert RAW_STYLE in styles_of(text, "ESP-ROM")


def test_command_reply_and_dump_markers():
    reply = render(CommandReply(text="level APP D ok"))
    assert reply.plain == "#> level APP D ok"
    assert REPLY_STYLE in styles_of(reply, "level APP D ok")
    begin = render(DumpBegin(filename="log.0", size=4096))
    assert begin.plain == "#BEGIN log.0 4096"
    assert DUMP_STYLE in styles_of(begin, "log.0")
    end = render(DumpEnd(crc32=0x00A1B2C3))
    assert end.plain == "#END 00a1b2c3"
    assert DUMP_STYLE in styles_of(end, "00a1b2c3")


# ---------------------------------------------------------------- invalid lines


def test_invalid_line_shows_reason_and_escaped_bytes():
    raw = b"0042:000137 00001\xff3456 I APP: x\n"
    text = render(InvalidLine(reason=InvalidReason.GARBAGE, raw=raw))
    assert text.plain == "!! garbage: 0042:000137 00001\\xff3456 I APP: x"
    assert INVALID_LABEL_STYLE in styles_of(text, "!! garbage:")
    assert ESCAPE_STYLE in styles_of(text, "\\xff")


def test_torn_line_sentinel_is_visible_and_newline_dropped():
    text = render(InvalidLine(reason=InvalidReason.TORN, raw=b"0042:0001\x7f\n"))
    assert text.plain == "!! torn: 0042:0001\\x7f"


def test_unterminated_invalid_line():
    text = render(InvalidLine(reason=InvalidReason.TORN, raw=b"0042:00"))
    assert text.plain == "!! torn: 0042:00"


def test_invalid_line_escapes_every_non_ascii_byte():
    raw = bytes(range(256))
    plain = render(InvalidLine(reason=InvalidReason.GARBAGE, raw=raw)).plain
    assert plain.isascii()
    assert all(0x20 <= ord(c) <= 0x7E for c in plain)
    assert "\\x00" in plain and "\\x0a" in plain and "\\xe9" in plain


# ---------------------------------------------------------------- device text is data


def test_markup_from_the_device_is_literal():
    text = render(log_line(msg="[bold red]x[/] [link=http://evil]y[/link]"))
    assert text.plain.endswith("[bold red]x[/] [link=http://evil]y[/link]")
    assert "bold red" not in {str(span.style) for span in text.spans}
    out = printed(text, terminal=False)
    assert "[bold red]x[/]" in out
    assert "[link=http://evil]" in out


@pytest.mark.parametrize(
    ("raw", "shown"),
    [
        ("a\x1bb", "a\\x1bb"),  # lone ESC left after the parser strips ANSI sequences
        ("bell\x07", "bell\\x07"),
        ("tab\there", "tab\\x09here"),
        ("del\x7f", "del\\x7f"),
        ("c1\x9b", "c1\\x9b"),  # C1 CSI: also starts an escape sequence on some terminals
        ("rtl\u202eevil", "rtl\\u202Eevil"),  # bidi override reorders text on screen
        ("zw\u200bsp", "zw\\u200Bsp"),
        ("ls\u2028x", "ls\\u2028x"),
    ],
)
def test_control_and_format_characters_are_escaped(raw, shown):
    text = render(RawLine(text=raw))
    assert text.plain == shown
    assert {str(span.style) for span in text.spans} == {RAW_STYLE, ESCAPE_STYLE}


def test_escape_spans_cover_exactly_the_escape():
    text = render(RawLine(text="a\x1bb"))
    assert text.spans == [
        Span(0, 1, RAW_STYLE),
        Span(1, 5, ESCAPE_STYLE),
        Span(5, 6, RAW_STYLE),
    ]


def test_consecutive_escapes_share_one_span():
    text = render(RawLine(text="x" + "\x00\x01\x02" + "y"))
    assert text.plain == "x\\x00\\x01\\x02y"
    assert [str(span.style) for span in text.spans] == [RAW_STYLE, ESCAPE_STYLE, RAW_STYLE]


@pytest.mark.parametrize(
    ("char", "shown"),
    [
        (chr(0xAD), "\\xad"),  # soft hyphen, Cf, last one-byte code: lowercase \x
        (chr(0x600), "\\u0600"),  # Arabic number sign, Cf, first two-byte code
        (chr(0xFEFF), "\\uFEFF"),  # BOM / zero-width no-break space
        (chr(0xE0041), "\\U000E0041"),  # tag character, Cf, beyond the BMP
        (chr(0xDC80), "\\uDC80"),  # lone surrogate from a surrogateescape decode
    ],
)
def test_escape_formats_by_code_point_range(char, shown):
    assert render(RawLine(text=f"<{char}>")).plain == f"<{shown}>"


def test_lone_surrogate_can_be_printed():
    text = render(RawLine(text="bad " + chr(0xDCFF)))
    assert "\\uDCFF" in printed(text, terminal=False)


def test_escape_in_every_device_field():
    text = render(IdfLine(level="I", timestamp_ms=1, tag="t\x1b", msg="m\x1b"))
    assert text.plain == "I (1) t\\x1b: m\\x1b"
    reply = render(CommandReply(text="ok\x1b[2J"))
    assert reply.plain == "#> ok\\x1b[2J"


# ---------------------------------------------------------------- parse_line -> render


def test_idf_line_with_ansi_colors_renders_without_them():
    text = render(parse_line(b"\x1b[0;31mE (1234) wifi: connection lost\x1b[0m\n"))
    assert text.plain == "E (1234) wifi: connection lost"
    assert LEVEL_STYLES["E"] in styles_of(text, "connection lost")


def test_osc_sequence_reaches_the_screen_escaped():
    # Not a CSI, so the parser keeps it; it would set the terminal title if printed raw.
    text = render(parse_line(b"\x1b]0;owned\x07\n"))
    assert text.plain == "\\x1b]0;owned\\x07"


def test_truncated_line_from_the_parser():
    line = b"0001:000002 0000000003 I APP: " + b"z" * 124 + b"...\n"
    parsed = parse_line(line)
    assert isinstance(parsed, LogLine) and parsed.truncated
    text = render(parsed)
    assert text.plain == line.decode().rstrip("\n")
    assert MARKER_STYLE in styles_of(text, "...")


def test_printable_unicode_is_kept():
    for value in ("ação ç", "temp 25 °C", "\ufffd replaced byte", "nbsp\u00a0here"):
        assert render(RawLine(text=value)).plain == value


def test_no_ansi_codes_when_not_a_terminal():
    text = render(log_line(level="E", msg="boom"))
    out = printed(text, terminal=False)
    assert "\x1b" not in out
    assert out == "0042:000137 0000123456 E APP: boom\n"


def test_ansi_codes_on_a_terminal():
    assert "\x1b[" in printed(render(log_line(level="E")), terminal=True)


@pytest.mark.parametrize(
    "line",
    [
        log_line(),
        InvalidLine(reason=InvalidReason.BAD_LEVEL, raw=b"x\n"),
        IdfLine(level="W", timestamp_ms=0, tag="T", msg=""),
        CommandReply(text=""),
        DumpBegin(filename="f", size=0),
        DumpEnd(crc32=0),
        RawLine(text=""),
    ],
)
def test_every_line_kind_renders(line):
    assert isinstance(render(line), Text)
