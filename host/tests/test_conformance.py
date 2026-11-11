"""The shared wire cases: log lines (conformance/line_cases.tsv, also read by the C++ suite),
console replies (conformance/reply_cases.tsv, ADR-012) and console requests
(conformance/request_cases.tsv, read by the C++ command parser).

assume_log=True: the parser's mode without heuristics, which returns exactly the firmware's
categories (the default mode turns non-esp32-logcat input into RawLine by design).
"""

import importlib.util
import re
import zlib
from pathlib import Path

import pytest

from esp_logcat.parser import (
    CommandEnd,
    CommandReply,
    DumpBegin,
    DumpEnd,
    InvalidLine,
    LogLine,
    RawLine,
    parse_line,
)

CONFORMANCE = Path(__file__).resolve().parents[2] / "conformance"
CASES_FILE = CONFORMANCE / "line_cases.tsv"
STATUSES = {"ok", "torn", "garbage", "bad_header", "bad_level", "overlong"}


def load_cases() -> list[list[str]]:
    rows = CASES_FILE.read_text(encoding="ascii").splitlines()
    return [row.split("\t") for row in rows if row and not row.startswith("#")]


CASES = load_cases()


def test_committed_file_matches_the_generator():
    # Nobody edits the TSV by hand: it must be exactly what make_line_cases.py writes.
    spec = importlib.util.spec_from_file_location(
        "make_line_cases", CONFORMANCE / "make_line_cases.py"
    )
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    assert CASES_FILE.read_bytes() == generator.render().encode("ascii")


def test_case_count_matches_the_header():
    declared = re.search(r"^# cases: (\d+)$", CASES_FILE.read_text(encoding="ascii"), re.M)
    assert declared is not None
    assert len(CASES) == int(declared[1])
    assert {row[1] for row in CASES} == STATUSES


@pytest.mark.parametrize("row", CASES, ids=[row[0] for row in CASES])
def test_python_parser_agrees(row):
    name, status, line_hex, *fields = row
    assert status in STATUSES, name
    assert len(row) == (10 if status == "ok" else 3), f"{name}: wrong column count"
    assert re.fullmatch(r"(?:[0-9a-f]{2})*", line_hex), f"{name}: bad hex"
    parsed = parse_line(bytes.fromhex(line_hex), assume_log=True)
    if status == "ok":
        boot, seq, uptime, level, tag, msg_hex, truncated = fields
        assert parsed == LogLine(
            boot=int(boot),
            seq=int(seq),
            uptime_ms=int(uptime),
            level=level,
            tag=tag,
            msg=bytes.fromhex(msg_hex).decode("ascii"),
            truncated=truncated == "1",
        ), name
    else:
        assert isinstance(parsed, InvalidLine), name
        assert parsed.reason == status, name


# ---------------------------------------------------------------- console replies (ADR-012)

REPLY_FILE = CONFORMANCE / "reply_cases.tsv"
REPLY_KINDS = {"data": 5, "end": 6, "dump_begin": 6, "dump_end": 5, "raw": 3, "block_crc": 4}


def load_reply_cases() -> list[list[str]]:
    rows = REPLY_FILE.read_text(encoding="ascii").splitlines()
    return [row.split("\t") for row in rows if row and not row.startswith("#")]


REPLY_CASES = load_reply_cases()


def test_committed_reply_file_matches_the_generator():
    spec = importlib.util.spec_from_file_location(
        "make_reply_cases", CONFORMANCE / "make_reply_cases.py"
    )
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    assert REPLY_FILE.read_bytes() == generator.render().encode("ascii")


def test_reply_case_count_matches_the_header():
    declared = re.search(r"^# cases: (\d+)$", REPLY_FILE.read_text(encoding="ascii"), re.M)
    assert declared is not None
    assert len(REPLY_CASES) == int(declared[1])
    assert {row[1] for row in REPLY_CASES} == set(REPLY_KINDS)


@pytest.mark.parametrize("row", REPLY_CASES, ids=[row[0] for row in REPLY_CASES])
def test_python_parser_agrees_on_replies(row):
    name, kind, line_hex, *fields = row
    assert len(row) == REPLY_KINDS[kind], f"{name}: wrong column count"
    if kind == "block_crc":
        # Block content, not a line: the host's dump reader (#29) checks it with zlib.crc32.
        assert f"{zlib.crc32(bytes.fromhex(line_hex)):08x}" == fields[0], name
        return
    parsed = parse_line(bytes.fromhex(line_hex))
    match kind:
        case "data":
            ident, joined = fields
            pairs = tuple(tuple(item.split("=", 1)) for item in joined.split(" "))
            assert parsed == CommandReply(id=ident, fields=pairs), name
        case "end":
            ident, code, text_hex = fields
            text = bytes.fromhex(text_hex).decode("ascii")
            assert parsed == CommandEnd(id=ident, code=code, text=text), name
        case "dump_begin":
            ident, filename, size = fields
            assert parsed == DumpBegin(id=ident, filename=filename, size=int(size)), name
        case "dump_end":
            ident, crc = fields
            assert parsed == DumpEnd(id=ident, crc32=int(crc, 16)), name
        case _:
            assert isinstance(parsed, RawLine), f"{name}: {parsed!r}"


# ------------------------------------------------------------------ console requests (#27)
# The host only writes requests; the C++ command parser is what reads this file
# (firmware/test/test_command_parser). Here: nobody edits the TSV by hand.

REQUEST_FILE = CONFORMANCE / "request_cases.tsv"


def test_committed_request_file_matches_the_generator():
    spec = importlib.util.spec_from_file_location(
        "make_request_cases", CONFORMANCE / "make_request_cases.py"
    )
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    assert REQUEST_FILE.read_bytes() == generator.render().encode("ascii")
