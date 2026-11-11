"""Writes conformance/request_cases.tsv: console request lines (wire-format section 2.1).

Usage (repository root): python conformance/make_request_cases.py

The expectations below are written by hand from the spec; no parser is the oracle. The C++
command parser (firmware/test/test_command_parser) reads the generated file now and the Rust
firmware (ADR-011) will read it too. The host only writes requests, so the Python suite just
checks that the committed file is what this script writes.

TSV columns: name, status, line_hex, id, kind, tag, level
- status: ok, empty (nothing to answer), bad_id, bad_command, bad_args. "bad_id" is the
          parser's name for "the first word is not an id"; on the wire it is answered with the
          code bad_args and the id "-", like a damaged line (wire-format 2.1).
- id:     the id to echo in the reply: the request's own, or "-" when it had no usable id
          (bad_id, a control byte, an overlong line). Empty column for status "empty".
- kind:   info, stats, log_dump, log_tail, log_clear, log_level, reboot, crash_panic, crash_wdt
          (only for status ok)
- tag, level: only for log_level; level is one of E W I D V N (N silences the tag)
Lines starting with '#' are comments.
"""

from pathlib import Path

OUT = Path(__file__).with_name("request_cases.tsv")
MAX_REQUEST = 64  # bytes before the line ending


def ok(name: str, line: bytes, ident: str, kind: str, tag: str = "", level: str = "") -> tuple:
    return (name, "ok", line, ident, kind, tag, level)


def bad(name: str, status: str, line: bytes, ident: str = "-") -> tuple:
    return (name, status, line, "" if status == "empty" else ident, "", "", "")


LONGEST = b"9999 log level " + b"t" * (MAX_REQUEST - len("9999 log level  N")) + b" N"
assert len(LONGEST) == MAX_REQUEST

CASES = [
    # ---------------------------------------------------------------- every command
    ok("info", b"7 info\n", "7", "info"),
    ok("stats", b"7 stats\n", "7", "stats"),
    ok("log_dump", b"7 log dump\n", "7", "log_dump"),
    ok("log_tail", b"7 log tail\n", "7", "log_tail"),
    ok("log_clear", b"7 log clear\n", "7", "log_clear"),
    ok("log_level", b"3 log level PRESSURE D\n", "3", "log_level", "PRESSURE", "D"),
    ok("reboot", b"7 reboot\n", "7", "reboot"),
    ok("crash_panic", b"7 crash panic\n", "7", "crash_panic"),
    ok("crash_wdt", b"7 crash wdt\n", "7", "crash_wdt"),
    # ---------------------------------------------------------------- ids
    ok("id_manual", b"- info\n", "-", "info"),
    ok("id_one", b"1 info\n", "1", "info"),
    ok("id_max", b"9999 info\n", "9999", "info"),
    bad("id_zero", "bad_id", b"0 info\n"),
    bad("id_leading_zero", "bad_id", b"07 info\n"),
    bad("id_five_digits", "bad_id", b"10000 info\n"),
    bad("id_negative", "bad_id", b"-1 info\n"),
    bad("id_double_dash", "bad_id", b"-- info\n"),
    bad("id_letters", "bad_id", b"x7 info\n"),
    bad("id_missing", "bad_id", b"info\n"),  # the first word is always the id
    bad("id_plus", "bad_id", b"+7 info\n"),
    bad("id_digit_then_letter", "bad_id", b"7x info\n"),
    bad("id_letter_inside", "bad_id", b"12a4 info\n"),
    bad("id_exponent", "bad_id", b"1e3 info\n"),
    ok("id_manual_with_arguments", b"- log level * W\n", "-", "log_level", "*", "W"),
    # ---------------------------------------------------------------- line endings and spaces
    ok("ending_crlf", b"7 info\r\n", "7", "info"),
    ok("ending_cr", b"7 info\r", "7", "info"),
    ok("ending_none", b"7 info", "7", "info"),
    ok("spaces_between", b"7   log    level  APP   W\n", "7", "log_level", "APP", "W"),
    ok("spaces_around", b"  7 info  \n", "7", "info"),
    bad("empty", "empty", b""),
    bad("empty_newline", "empty", b"\n"),
    bad("empty_crlf", "empty", b"\r\n"),
    bad("empty_spaces", "empty", b"   \n"),
    bad("ending_lfcr", "bad_args", b"7 info\n\r"),  # the LF is then a control byte in the line
    bad("ending_two_newlines", "bad_args", b"7 info\n\n"),
    bad("tab_separator", "bad_args", b"7\tinfo\n"),
    bad("control_byte", "bad_args", b"7 info\x1b[A\n"),
    bad("nul_byte", "bad_args", b"7 info\x00\n"),
    bad("del_byte", "bad_args", b"7 info\x7f\n"),
    bad("non_ascii", "bad_args", b"7 inf\xc3\xb3\n"),
    ok("longest_line", LONGEST + b"\n", "9999", "log_level", "t" * 15, "N"),
    ok("longest_line_crlf", LONGEST + b"\r\n", "9999", "log_level", "t" * 15, "N"),
    bad("overlong_line", "bad_args", LONGEST[:-2] + b"t N\n"),
    bad("overlong_line_crlf", "bad_args", LONGEST[:-2] + b"t N\r\n"),
    # Spaces around the request count for the limit; the line ending does not.
    ok("limit_with_spaces", b"7 info".ljust(MAX_REQUEST) + b"\n", "7", "info"),
    bad("over_limit_with_spaces", "bad_args", b"7 info".ljust(MAX_REQUEST + 1) + b"\n"),
    bad("blank_overlong", "empty", b" " * (MAX_REQUEST + 6) + b"\n"),  # blank wins: no reply
    bad("cr_in_the_middle", "bad_args", b"7 in\rfo\n"),
    bad("ending_two_crs", "bad_args", b"7 info\r\r"),
    ok("crash_panic_crlf", b"7 crash panic\r\n", "7", "crash_panic"),
    # ---------------------------------------------------------------- unknown commands
    bad("only_id", "bad_command", b"7\n", "7"),
    bad("unknown_command", "bad_command", b"7 help\n", "7"),
    bad("unknown_manual_id", "bad_command", b"- help\n", "-"),
    bad("uppercase_command", "bad_command", b"7 INFO\n", "7"),
    bad("prefix_of_command", "bad_command", b"7 inf\n", "7"),
    bad("command_with_suffix", "bad_command", b"7 infos\n", "7"),
    bad("log_unknown_subcommand", "bad_command", b"7 log grep\n", "7"),
    bad("log_uppercase_subcommand", "bad_command", b"7 log DUMP\n", "7"),
    bad("log_uppercase", "bad_command", b"7 LOG dump\n", "7"),
    bad("log_level_uppercase", "bad_command", b"7 log LEVEL APP D\n", "7"),
    bad("unknown_with_arguments", "bad_command", b"7 help me\n", "7"),
    bad("log_unknown_with_arguments", "bad_command", b"7 log grep x\n", "7"),
    bad("many_unknown_words", "bad_command", b"7 " + b" ".join([b"a"] * 31) + b"\n", "7"),
    # ---------------------------------------------------------------- wrong arguments
    bad("info_extra", "bad_args", b"7 info now\n", "7"),
    bad("stats_extra", "bad_args", b"7 stats 1\n", "7"),
    bad("reboot_extra", "bad_args", b"7 reboot now\n", "7"),
    bad("log_alone", "bad_args", b"7 log\n", "7"),
    bad("log_dump_extra", "bad_args", b"7 log dump all\n", "7"),
    bad("log_tail_extra", "bad_args", b"7 log tail 10\n", "7"),
    bad("log_clear_extra", "bad_args", b"7 log clear all\n", "7"),
    bad("crash_alone", "bad_args", b"7 crash\n", "7"),
    bad("crash_unknown", "bad_args", b"7 crash now\n", "7"),
    bad("crash_uppercase", "bad_args", b"7 crash PANIC\n", "7"),
    bad("crash_extra", "bad_args", b"7 crash panic now\n", "7"),
    bad("info_many_extra", "bad_args", b"7 info a b c d e f g\n", "7"),
    # ---------------------------------------------------------------- log level
    ok("level_error", b"7 log level APP E\n", "7", "log_level", "APP", "E"),
    ok("level_warn", b"7 log level APP W\n", "7", "log_level", "APP", "W"),
    ok("level_info", b"7 log level APP I\n", "7", "log_level", "APP", "I"),
    ok("level_verbose", b"7 log level APP V\n", "7", "log_level", "APP", "V"),
    ok("level_none", b"7 log level wifi N\n", "7", "log_level", "wifi", "N"),
    ok("level_default", b"7 log level * W\n", "7", "log_level", "*", "W"),
    ok("level_tag_15", b"7 log level " + b"t" * 15 + b" D\n", "7", "log_level", "t" * 15, "D"),
    ok(  # cut to the 15 chars a record keeps, so it matches the stored tag
        "level_tag_cut_to_15",
        b"7 log level esp-x509-crt-bundle D\n",
        "7",
        "log_level",
        "esp-x509-crt-bu",
        "D",
    ),
    ok("level_tag_punctuation", b"7 log level a:b/c D\n", "7", "log_level", "a:b/c", "D"),
    ok("level_tag_one_char", b"7 log level x D\n", "7", "log_level", "x", "D"),
    # The tag is compared with what the code passed to the log, not with what the line shows
    # (there, space and ':' print as '_'): no translation here.
    ok("level_tag_underscore", b"7 log level a_b D\n", "7", "log_level", "a_b", "D"),
    ok("level_tag_dash", b"7 log level - D\n", "7", "log_level", "-", "D"),
    ok("level_tag_two_stars", b"7 log level ** D\n", "7", "log_level", "**", "D"),  # a tag
    ok("level_tag_star_prefix", b"7 log level *a D\n", "7", "log_level", "*a", "D"),
    ok(
        "level_tag_cut_at_punctuation",
        b"7 log level 0123456789abcd:xyz D\n",
        "7",
        "log_level",
        "0123456789abcd:",
        "D",
    ),
    bad("level_many_extra", "bad_args", b"7 log level APP D a b c\n", "7"),
    bad("level_missing_both", "bad_args", b"7 log level\n", "7"),
    bad("level_missing_level", "bad_args", b"7 log level APP\n", "7"),
    bad("level_extra", "bad_args", b"7 log level APP D now\n", "7"),
    bad("level_lowercase", "bad_args", b"7 log level APP d\n", "7"),
    bad("level_word", "bad_args", b"7 log level APP DEBUG\n", "7"),
    bad("level_unknown_letter", "bad_args", b"7 log level APP X\n", "7"),
    bad("level_silent_s", "bad_args", b"7 log level APP S\n", "7"),  # host filters use S; not here
    bad("level_question_mark", "bad_args", b"7 log level APP ?\n", "7"),
    bad("level_digit", "bad_args", b"7 log level APP 3\n", "7"),
]


def render() -> str:
    """The TSV text. The Python suite compares it with the committed file (no hand edits)."""
    names = [case[0] for case in CASES]
    assert len(names) == len(set(names)), "case names must be unique"
    rows = [
        "# Generated by conformance/make_request_cases.py: edit that file, not this one.",
        "# version: 1 (console requests, wire-format section 2.1, proto=1)",
        f"# cases: {len(CASES)}",
        "# name\tstatus\tline_hex\tid\tkind\ttag\tlevel",
    ]
    for name, status, line, ident, kind, tag, level in CASES:
        assert (kind != "") == (status == "ok"), name
        assert (tag != "") == (kind == "log_level") == (level != ""), name
        rows.append("\t".join([name, status, line.hex(), ident, kind, tag, level]))
    return "\n".join(rows) + "\n"


def main() -> None:
    OUT.write_text(render(), encoding="ascii", newline="\n")
    print(f"{OUT}: {len(CASES)} cases")


if __name__ == "__main__":
    main()
