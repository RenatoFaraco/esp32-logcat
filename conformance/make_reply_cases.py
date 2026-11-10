"""Writes conformance/reply_cases.tsv: the console reply lines (ADR-012) both sides must agree on.

Usage (repository root): python conformance/make_reply_cases.py

The expectations below are written by hand from ADR-012; no parser is the oracle. The Python
suite (host/tests/test_conformance.py) reads the generated file now; the C++ reply formatter
(#27/#28) and the Go host (ADR-010) will read it too. Changing the reply format means changing
ADR-012 and this file, regenerating the TSV and making every side pass again, in one commit.

TSV columns: name, kind, line_hex, then per kind:
- data:       id, fields ("key=value" joined by single spaces: values never contain a space)
- end:        id, code ("ok" on success), text_hex
- dump_begin: id, name, size
- dump_end:   id, crc32 (8 lowercase hex digits)
- raw:        nothing (a malformed protocol line is shown and counted, never half-read)
- block_crc:  crc32 of the bytes in line_hex, which here is the *content* of a dump block, not
              a line: sent verbatim, with flash garbage, DEL sentinels and no final newline
              (ADR-012 section 3). CRC-32 IEEE, the one of zlib.crc32; the expected values are
              literals, and "123456789" -> cbf43926 is the standard check value.
Lines starting with '#' are comments.
"""

from pathlib import Path

OUT = Path(__file__).with_name("reply_cases.tsv")
MAX_LINE = 170  # bytes with the final newline, like a log line (wire-format, ADR-012)


def ln(text: str) -> bytes:
    return (text + "\n").encode("ascii")


KEY32 = "k" * 32
# What a dump block may carry from flash: a good line, a torn one closed with DEL + newline
# (PRD section 3), an erased-sector run (0xFF), a NUL and 200 bytes with no newline at all.
BLOCK_CONTENT = (
    ln("0042:000001 0000000010 I APP: ok")
    + b"0042:000002 0000000011 W APP: cut"
    + bytes([0x7F, 0x0A])
    + bytes([0xFF]) * 4
    + bytes([0x00])
    + b"x" * 200
)
assert len(BLOCK_CONTENT) == 273
DATA170 = "#> 9999 " + "v=" + "x" * (MAX_LINE - len("#> 9999 v=") - 1)
assert len(ln(DATA170)) == MAX_LINE
ERROR170 = "#! 1 failed " + "y" * (MAX_LINE - len("#! 1 failed ") - 1)
assert len(ln(ERROR170)) == MAX_LINE

# (name, line bytes, kind, expected fields or None)
CASES = [
    # ---------------------------------------------------------------- data lines
    ("data_one_field", ln("#> 7 proto=1"), "data", ("7", "proto=1")),
    (
        "data_info",
        ln("#> 7 fw=0.3.0 proto=1 chip=esp32c3 uptime_ms=5123 reset=POWERON"),
        "data",
        ("7", "fw=0.3.0 proto=1 chip=esp32c3 uptime_ms=5123 reset=POWERON"),
    ),
    ("data_dotted_key", ln("#> 12 not_sent.usb=0"), "data", ("12", "not_sent.usb=0")),
    ("data_manual_id", ln("#> - dropped=72"), "data", ("-", "dropped=72")),
    ("data_max_id", ln("#> 9999 a=1"), "data", ("9999", "a=1")),
    ("data_value_with_equals", ln("#> 1 expr=a=b"), "data", ("1", "expr=a=b")),
    ("data_value_punctuation", ln("#> 1 queue=3/32"), "data", ("1", "queue=3/32")),
    ("data_key_32_chars", ln(f"#> 1 {KEY32}=1"), "data", ("1", f"{KEY32}=1")),
    ("data_170_bytes", ln(DATA170), "data", ("9999", DATA170.removeprefix("#> 9999 "))),
    # ---------------------------------------------------------------- terminators
    ("end_ok", ln("#. 7 ok"), "end", ("7", "ok", "")),
    ("end_ok_manual_id", ln("#. - ok"), "end", ("-", "ok", "")),
    (
        "end_error_with_text",
        ln("#! 3 full tag table full (16 tags)"),
        "end",
        ("3", "full", "tag table full (16 tags)"),
    ),
    ("end_error_without_text", ln("#! 3 bad_args"), "end", ("3", "bad_args", "")),
    (
        "end_error_unknown_code",
        ln("#! 3 low_battery not now"),
        "end",
        ("3", "low_battery", "not now"),
    ),
    # ---------------------------------------------------------------- dump block markers
    ("dump_begin", ln("#BEGIN 4 log_003.txt 4096"), "dump_begin", ("4", "log_003.txt", "4096")),
    ("dump_begin_ram", ln("#BEGIN 4 ram 0"), "dump_begin", ("4", "ram", "0")),
    ("dump_end", ln("#END 4 1a2b3c4d"), "dump_end", ("4", "1a2b3c4d")),
    ("dump_end_upper_hex", ln("#END 4 1A2B3C4D"), "dump_end", ("4", "1a2b3c4d")),
    # ---------------------------------------------------------------- malformed: raw
    ("raw_no_id", ln("#> fw=0.3.0"), "raw", None),
    ("raw_id_zero", ln("#> 0 a=1"), "raw", None),
    ("raw_id_leading_zero", ln("#> 07 a=1"), "raw", None),
    ("raw_id_five_digits", ln("#> 10000 a=1"), "raw", None),
    ("raw_data_without_fields", ln("#> 7"), "raw", None),
    ("raw_data_trailing_space", ln("#> 7 a=1 "), "raw", None),
    ("raw_field_without_equals", ln("#> 7 hello"), "raw", None),
    ("raw_empty_value", ln("#> 7 a="), "raw", None),
    ("raw_uppercase_key", ln("#> 7 Fw=1"), "raw", None),
    ("raw_key_starts_with_digit", ln("#> 7 1a=1"), "raw", None),
    ("raw_key_33_chars", ln(f"#> 7 {KEY32}k=1"), "raw", None),
    ("raw_repeated_key", ln("#> 7 a=1 a=2"), "raw", None),
    ("raw_double_space", ln("#> 7 a=1  b=2"), "raw", None),
    ("raw_171_bytes", ln(DATA170 + "x"), "raw", None),
    ("raw_no_newline", b"#> 7 a=1", "raw", None),
    ("raw_carriage_return", b"#> 7 a=1\r\n", "raw", None),
    ("raw_control_byte", ln("#> 7 a=1\x1b[2J"), "raw", None),
    ("raw_non_ascii_byte", b"#> 7 a=\xc3\xa9\n", "raw", None),
    ("raw_ok_with_extra", ln("#. 7 ok now"), "raw", None),
    ("raw_ok_word_wrong", ln("#. 7 done"), "raw", None),
    ("raw_error_without_code", ln("#! 3"), "raw", None),
    ("raw_error_code_uppercase", ln("#! 3 FULL table"), "raw", None),
    ("raw_error_code_ok", ln("#! 3 ok but not really"), "raw", None),  # success is only "#."
    ("raw_error_text_control_byte", ln("#! 3 failed bad\x1b[2J"), "raw", None),
    ("raw_error_text_non_ascii", b"#! 3 failed caf\xc3\xa9\n", "raw", None),
    ("raw_old_reply_without_id", ln("#> boot=42 reason=PANIC fw=0.3.0"), "raw", None),
    ("raw_old_dump_begin_without_id", ln("#BEGIN log_003.txt 4096"), "raw", None),
    ("raw_old_dump_end_without_id", ln("#END 1a2b3c4d"), "raw", None),
    ("raw_dump_end_short_crc", ln("#END 4 1a2b3c4"), "raw", None),
    ("raw_dump_begin_bad_name", ln("#BEGIN 4 ../etc 10"), "raw", None),
    ("raw_unknown_hash_prefix", ln("#? 7 a=1"), "raw", None),
    # ---------------------------------------------------------------- review of #69
    (
        "end_error_text_looks_like_log_separators",  # spaces at 11, 22 and 24
        ln("#! 3 failed erase page 7 of 64"),
        "end",
        ("3", "failed", "erase page 7 of 64"),
    ),
    ("end_error_code_32_chars", ln("#! 3 " + "e" * 32), "end", ("3", "e" * 32, "")),
    ("end_error_code_underscore", ln("#! 3 _"), "end", ("3", "_", "")),
    ("end_error_manual_id", ln("#! - full"), "end", ("-", "full", "")),
    ("end_error_170_bytes", ln(ERROR170), "end", ("1", "failed", ERROR170[12:])),
    ("dump_end_manual_id", ln("#END - 1a2b3c4d"), "dump_end", ("-", "1a2b3c4d")),
    ("dump_begin_name_64", ln("#BEGIN 4 " + "n" * 64 + " 1"), "dump_begin", ("4", "n" * 64, "1")),
    ("dump_begin_name_dots_inside", ln("#BEGIN 4 a..b 1"), "dump_begin", ("4", "a..b", "1")),
    (
        "dump_begin_size_10_digits",
        ln("#BEGIN 4 f 4294967295"),
        "dump_begin",
        ("4", "f", "4294967295"),
    ),
    ("raw_error_trailing_space", ln("#! 3 full "), "raw", None),
    ("raw_error_code_33_chars", ln("#! 3 " + "e" * 33), "raw", None),
    ("raw_error_171_bytes", ln(ERROR170 + "x"), "raw", None),
    ("raw_end_id_zero", ln("#. 0 ok"), "raw", None),
    ("raw_error_id_leading_zero", ln("#! 07 full"), "raw", None),
    ("raw_dump_begin_id_five_digits", ln("#BEGIN 10000 f 1"), "raw", None),
    ("raw_id_double_dash", ln("#> -- a=1"), "raw", None),
    ("raw_id_negative", ln("#> -1 a=1"), "raw", None),
    ("raw_dump_begin_name_65", ln("#BEGIN 4 " + "n" * 65 + " 1"), "raw", None),
    ("raw_dump_begin_name_starts_with_dash", ln("#BEGIN 4 -f 1"), "raw", None),
    ("raw_dump_begin_size_11_digits", ln("#BEGIN 4 f 12345678901"), "raw", None),
    ("raw_dump_begin_size_leading_zero", ln("#BEGIN 4 f 0004"), "raw", None),
    ("raw_dump_end_9_hex", ln("#END 4 1a2b3c4d5"), "raw", None),
    ("raw_dump_end_not_hex", ln("#END 4 1a2b3c4g"), "raw", None),
    ("raw_key_starts_with_dot", ln("#> 7 .a=1"), "raw", None),
    ("raw_value_with_del", ln("#> 7 a=1\x7f"), "raw", None),
    ("raw_tab_separator", ln("#> 7\ta=1"), "raw", None),
    # ---------------------------------------------------------------- dump block content CRC
    ("block_crc_check_value", b"123456789", "block_crc", ("cbf43926",)),
    ("block_crc_empty", b"", "block_crc", ("00000000",)),
    ("block_crc_flash_content", BLOCK_CONTENT, "block_crc", ("1547ffef",)),
]


def render() -> str:
    """The TSV text. The Python suite compares it with the committed file (no hand edits)."""
    names = [case[0] for case in CASES]
    assert len(names) == len(set(names)), "case names must be unique"
    rows = [
        "# Generated by conformance/make_reply_cases.py: edit that file, not this one.",
        "# version: 1 (ADR-012 console reply framing, proto=1)",
        f"# cases: {len(CASES)}",
        "# Every line is classified as a protocol line or raw; nothing is half-parsed.",
        "# block_crc rows carry dump block content (verbatim bytes), not a line.",
        "# name\tkind\tline_hex\t<fields per kind, see make_reply_cases.py>",
    ]
    for name, data, kind, fields in CASES:
        assert (fields is None) == (kind == "raw"), name
        row = [name, kind, data.hex()]
        if kind == "end":
            ident, code, text = fields
            row += [ident, code, text.encode("ascii").hex()]
        elif fields is not None:
            row += list(fields)
        rows.append("\t".join(row))
    return "\n".join(rows) + "\n"


def main() -> None:
    OUT.write_text(render(), encoding="ascii", newline="\n")
    print(f"{OUT}: {len(CASES)} cases")


if __name__ == "__main__":
    main()
