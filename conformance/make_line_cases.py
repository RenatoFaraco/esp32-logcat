"""Writes conformance/line_cases.tsv: the line-format cases both parsers must agree on.

Usage (repository root): python conformance/make_line_cases.py

The expectations below are written by hand from PRD §3 (ADR-006, ADR-007); neither parser is
the oracle. The C++ suite (firmware/test/test_conformance) and the Python suite
(host/tests/test_conformance.py) read the generated file, and the Go host (ADR-010) will too.
Changing the line format means changing this file, regenerating the TSV, and making both
parsers pass again, in the same commit (CLAUDE.md).

TSV columns: name, status, line_hex, then for status "ok" the parsed fields:
boot, seq, uptime, level, tag, msg_hex, truncated (0/1). Lines starting with '#' are comments.
"""

from pathlib import Path

OUT = Path(__file__).with_name("line_cases.tsv")


def line(boot: int, seq: int, uptime: int, level: str, tag: str, msg: str) -> bytes:
    return f"{boot:04d}:{seq:06d} {uptime:010d} {level} {tag}: {msg}\n".encode("ascii")


NOMINAL = line(42, 137, 123456, "W", "PRESSURE", "reading out of range (4.71 V)")
TAG15 = "T" * 15
MSG127 = "m" * 127
LINE170 = line(1, 2, 3, "I", TAG15, MSG127)
assert len(LINE170) == 170

# (name, line bytes, status, (boot, seq, uptime, level, tag, msg, truncated) or None)
CASES = [
    # ---------------------------------------------------------------- valid
    (
        "nominal",
        NOMINAL,
        "ok",
        (42, 137, 123456, "W", "PRESSURE", "reading out of range (4.71 V)", False),
    ),
    ("all_zero", line(0, 0, 0, "I", "A", "x"), "ok", (0, 0, 0, "I", "A", "x", False)),
    (
        "max_fields",
        line(9999, 999999, 4294967295, "E", "APP", "max"),
        "ok",
        (9999, 999999, 4294967295, "E", "APP", "max", False),
    ),
    (
        "level_d",
        line(1, 1, 1, "D", "APP", "debug"),
        "ok",
        (1, 1, 1, "D", "APP", "debug", False),
    ),
    (
        "level_v",
        line(1, 1, 1, "V", "APP", "verbose"),
        "ok",
        (1, 1, 1, "V", "APP", "verbose", False),
    ),
    (
        "empty_message",
        line(1, 1, 1, "I", "APP", ""),
        "ok",
        (1, 1, 1, "I", "APP", "", False),
    ),
    (
        "dash_tag",
        line(1, 1, 1, "I", "-", "empty tag prints -"),
        "ok",
        (1, 1, 1, "I", "-", "empty tag prints -", False),
    ),
    (
        "tag_15_chars",
        line(1, 1, 1, "I", TAG15, "x"),
        "ok",
        (1, 1, 1, "I", TAG15, "x", False),
    ),
    (
        "edge_spaces_kept",
        line(1, 1, 1, "I", "APP", " padded "),
        "ok",
        (1, 1, 1, "I", "APP", " padded ", False),
    ),
    (
        "colon_in_message",
        line(1, 1, 1, "I", "APP", "a: b: c"),
        "ok",
        (1, 1, 1, "I", "APP", "a: b: c", False),
    ),
    (
        "truncated_124_plus_dots",
        line(1, 1, 1, "W", "APP", "a" * 124 + "..."),
        "ok",
        (1, 1, 1, "W", "APP", "a" * 124 + "...", True),
    ),
    (
        "short_message_with_dots_not_truncated",
        line(1, 1, 1, "I", "APP", "wait..."),
        "ok",
        (1, 1, 1, "I", "APP", "wait...", False),
    ),
    ("max_length_170", LINE170, "ok", (1, 2, 3, "I", TAG15, MSG127, False)),
    # ---------------------------------------------------------------- torn
    ("torn_no_newline", NOMINAL[:-1], "torn", None),
    ("torn_header_only", b"0042:0001", "torn", None),
    ("torn_empty", b"", "torn", None),
    ("torn_closed_with_del", NOMINAL[:-1] + b"\x7f\n", "torn", None),
    ("torn_closed_with_del_171_bytes", LINE170[:-1] + b"\x7f\n", "torn", None),
    # ---------------------------------------------------------------- overlong
    ("overlong_171_bytes", line(1, 2, 3, "I", TAG15, MSG127 + "m"), "overlong", None),
    ("overlong_message_128", line(1, 1, 1, "I", "A", "m" * 128), "overlong", None),
    # ---------------------------------------------------------------- garbage
    ("nul_in_message", line(1, 1, 1, "I", "APP", "a\x00b"), "garbage", None),
    ("nul_before_newline", NOMINAL[:-1] + b"\x00\n", "garbage", None),
    ("ff_in_tag", NOMINAL[:30] + b"\xff" + NOMINAL[31:], "garbage", None),
    (
        "ff_in_message",
        line(1, 1, 1, "I", "APP", "a").replace(b"a\n", b"a\xff\n"),
        "garbage",
        None,
    ),
    ("ff_at_start", b"\xff" + NOMINAL[1:], "garbage", None),
    (
        "byte_0x80",
        line(1, 1, 1, "I", "APP", "a").replace(b"a\n", b"\x80\n"),
        "garbage",
        None,
    ),
    (
        "utf8_in_message",
        line(1, 1, 1, "I", "APP", "caf").replace(b"caf\n", b"caf\xc3\xa9\n"),
        "garbage",
        None,
    ),
    ("nul_at_start", b"\x00" + NOMINAL[1:], "garbage", None),
    ("nul_in_boot", b"00\x0042" + NOMINAL[4:], "garbage", None),
    ("nul_in_tag", NOMINAL.replace(b"PRESS", b"PR\x00SS"), "garbage", None),
    ("only_nul", b"\x00" * 60 + b"\n", "garbage", None),
    ("two_lines_glued", NOMINAL + NOMINAL, "garbage", None),  # '\n' in the middle
    (
        "garbage_beats_bad_header",
        b"XXXX:000001 0000000001 I APP: \xff\n",
        "garbage",
        None,
    ),
    ("erased_flash_run", b"\xff" * 40 + b"\n", "garbage", None),
    ("lone_cr", line(1, 1, 1, "I", "APP", "a\rb"), "garbage", None),
    ("crlf_ending", NOMINAL[:-1] + b"\r\n", "garbage", None),
    ("del_in_middle", line(1, 1, 1, "I", "APP", "a\x7fb"), "garbage", None),
    ("tab_in_message", line(1, 1, 1, "I", "APP", "a\tb"), "garbage", None),
    # ---------------------------------------------------------------- bad header
    ("uptime_over_32_bits", b"0001:000001 4294967296 I APP: x\n", "bad_header", None),
    ("uptime_negative", b"0001:000001 -000000001 I APP: x\n", "bad_header", None),
    ("uptime_plus_sign", b"0001:000001 +000000001 I APP: x\n", "bad_header", None),
    ("uptime_hex", b"0001:000001 0x00000001 I APP: x\n", "bad_header", None),
    ("seq_short", b"0001:00001 0000000001 I APP: x\n", "bad_header", None),
    (
        "missing_colon_after_boot",
        b"0001-000001 0000000001 I APP: x\n",
        "bad_header",
        None,
    ),
    ("no_space_after_colon", b"0001:000001 0000000001 I APP:x\n", "bad_header", None),
    (
        "empty_message_without_space",
        b"0001:000001 0000000001 I APP:\n",
        "bad_header",
        None,
    ),
    ("empty_tag", b"0001:000001 0000000001 I : x\n", "bad_header", None),
    ("tag_16_chars", line(1, 1, 1, "I", "T" * 16, "x"), "bad_header", None),
    ("space_in_tag", b"0001:000001 0000000001 I MY TAG: x\n", "bad_header", None),
    ("no_colon_at_all", b"0001:000001 0000000001 I APP x\n", "bad_header", None),
    ("line_28_bytes", b"0000:000000 0000000000 I A:\n", "bad_header", None),
    ("lone_newline", b"\n", "bad_header", None),
    # ---------------------------------------------------------------- bad level
    ("level_question_mark", line(1, 1, 1, "?", "APP", "x"), "bad_level", None),
    ("level_lowercase", line(1, 1, 1, "i", "APP", "x"), "bad_level", None),
    ("level_n_never_a_line_level", line(1, 1, 1, "N", "APP", "x"), "bad_level", None),
    ("level_two_letters", b"0001:000001 0000000001 II APP: x\n", "bad_header", None),
    (
        "bad_level_checked_before_tag",
        b"0001:000001 0000000001 ? MY TAG: x\n",
        "bad_level",
        None,
    ),
    (
        "bad_header_checked_before_level",
        b"0001:00001X 0000000001 ? APP: x\n",
        "bad_header",
        None,
    ),
    # ---------------------------------------------------------------- field widths, separators
    ("boot_3_digits", b"001:000001 0000000001 I APP: x\n", "bad_header", None),
    ("boot_5_digits", b"00001:000001 0000000001 I APP: x\n", "bad_header", None),
    ("seq_7_digits", b"0001:0000001 0000000001 I APP: x\n", "bad_header", None),
    ("uptime_9_digits", b"0001:000001 000000001 I APP: x\n", "bad_header", None),
    ("uptime_9999999999", b"0001:000001 9999999999 I APP: x\n", "bad_header", None),
    ("boot_space_padded", b" 042:000001 0000000001 I APP: x\n", "bad_header", None),
    ("seq_with_underscore", b"0001:00_001 0000000001 I APP: x\n", "bad_header", None),
    ("letter_in_boot", b"00a1:000001 0000000001 I APP: x\n", "bad_header", None),
    (
        "two_spaces_after_level",
        b"0001:000001 0000000001 I  APP: x\n",
        "bad_header",
        None,
    ),
    ("tab_instead_of_space", b"0001:000001\t0000000001 I APP: x\n", "garbage", None),
    ("leading_space", b" 0001:000001 0000000001 I APP: x\n", "bad_header", None),
    ("no_space_before_level", b"0001:000001 0000000001I APP: x\n", "bad_header", None),
    (
        "space_before_tag_colon",
        b"0001:000001 0000000001 I APP : x\n",
        "bad_header",
        None,
    ),
    (
        "two_spaces_after_colon",
        line(1, 1, 1, "I", "APP", " x"),
        "ok",
        (1, 1, 1, "I", "APP", " x", False),
    ),
    # ---------------------------------------------------------------- length and DEL boundaries
    (
        "min_line_29_bytes",
        line(0, 0, 0, "I", "A", ""),
        "ok",
        (0, 0, 0, "I", "A", "", False),
    ),
    (
        "length_169",
        line(1, 2, 3, "I", TAG15, MSG127[:-1]),
        "ok",
        (1, 2, 3, "I", TAG15, MSG127[:-1], False),
    ),
    ("torn_closed_with_del_170_bytes", LINE170[:-2] + b"\x7f\n", "torn", None),
    ("del_closed_172_bytes", LINE170 + b"\x7f\n", "overlong", None),
    ("only_del_newline", b"\x7f\n", "torn", None),
    ("only_del", b"\x7f", "torn", None),
    ("del_closing_garbage", b"\xff\x7f\n", "torn", None),  # the store's sentinel wins
    ("ff_run_171_with_newline", b"\xff" * 170 + b"\n", "overlong", None),
    ("ff_run_without_newline", b"\xff" * 40, "torn", None),  # an erased sector, cut
    ("nul_last_without_newline", NOMINAL[:-1] + b"\x00", "torn", None),
    # ---------------------------------------------------------------- what the formatter writes
    (
        "sanitized_question_mark_in_message",
        line(1, 1, 1, "I", "APP", "a?b"),
        "ok",
        (1, 1, 1, "I", "APP", "a?b", False),
    ),
    (
        "sanitized_underscore_in_tag",
        line(1, 1, 1, "I", "MY_TAG", "x"),
        "ok",
        (1, 1, 1, "I", "MY_TAG", "x", False),
    ),
    (
        "tag_with_dash_and_digits",
        line(1, 1, 1, "I", "sensor-2", "x"),
        "ok",
        (1, 1, 1, "I", "sensor-2", "x", False),
    ),
    (
        "message_126_ending_in_dots_not_truncated",
        line(1, 1, 1, "I", "APP", "a" * 123 + "..."),
        "ok",
        (1, 1, 1, "I", "APP", "a" * 123 + "...", False),
    ),
    (
        "seq_wrap_low_values_after_high",
        line(0, 0, 0, "I", "APP", "parser assumes no order"),
        "ok",
        (0, 0, 0, "I", "APP", "parser assumes no order", False),
    ),
]


def render() -> str:
    """The TSV text. The Python suite compares it with the committed file (no hand edits)."""
    names = [case[0] for case in CASES]
    assert len(names) == len(set(names)), "case names must be unique"
    rows = [
        "# Generated by conformance/make_line_cases.py: edit that file, not this one.",
        "# version: 1 (PRD section 3 line format)",
        f"# cases: {len(CASES)}",
        "# Precedence (both parsers): DEL+newline sentinel (torn) > over 170 bytes (overlong) >",
        "#   no final newline (torn) > byte outside 0x20-0x7E (garbage) > header > level > tag >",
        "#   message over 127 chars (overlong).",
        "# An empty input is torn; a lone newline is bad_header.",
        "# Strict device format only: the host's lenient mode (TAG: then newline) is not in here.",
        "# name\tstatus\tline_hex\tboot\tseq\tuptime\tlevel\ttag\tmsg_hex\ttruncated",
    ]
    for name, data, status, fields in CASES:
        if fields is not None:
            tag, msg = fields[4], fields[5]
            assert all(0x20 < ord(c) < 0x7F and c != ":" for c in tag), name
            assert all(0x20 <= ord(c) < 0x7F for c in msg), name
        row = [name, status, data.hex()]
        if fields is not None:
            boot, seq, uptime, level, tag, msg, truncated = fields
            row += [
                str(boot),
                str(seq),
                str(uptime),
                level,
                tag,
                msg.encode().hex(),
                str(int(truncated)),
            ]
        rows.append("\t".join(row))
    return "\n".join(rows) + "\n"


def main() -> None:
    OUT.write_text(render(), encoding="ascii", newline="\n")
    print(f"{OUT}: {len(CASES)} cases")


if __name__ == "__main__":
    main()
