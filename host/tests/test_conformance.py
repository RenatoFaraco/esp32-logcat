"""The shared line-format cases (conformance/line_cases.tsv), also read by the C++ suite.

assume_log=True: the parser's mode without heuristics, which returns exactly the firmware's
categories (the default mode turns non-esp32-logcat input into RawLine by design).
"""

import importlib.util
import re
from pathlib import Path

import pytest

from esp_logcat.parser import InvalidLine, LogLine, parse_line

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
