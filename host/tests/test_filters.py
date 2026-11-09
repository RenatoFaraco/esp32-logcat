import pytest

from esp_logcat.filters import FilterSpecError, LogFilter
from esp_logcat.parser import (
    CommandEnd,
    CommandReply,
    DumpBegin,
    DumpEnd,
    IdfLine,
    InvalidLine,
    InvalidReason,
    LogLine,
    RawLine,
)


def log(tag: str, level: str) -> LogLine:
    return LogLine(boot=0, seq=1, uptime_ms=1, level=level, tag=tag, msg="m", truncated=False)


def idf(tag: str, level: str) -> IdfLine:
    return IdfLine(level=level, timestamp_ms=1, tag=tag, msg="m")


def shown(specs: list[str], tag: str, levels: str = "EWIDV") -> str:
    """Levels of `tag` that pass, as a string: the compact form of the table below."""
    line_filter = LogFilter.parse(specs)
    return "".join(level for level in levels if line_filter.accepts(log(tag, level)))


# ---------------------------------------------------------------- the table (Done when of #17)

TABLE = [
    # specs                              tag          levels shown
    ([], "APP", "EWIDV"),
    (["*:V"], "APP", "EWIDV"),
    (["*:W"], "APP", "EW"),
    (["*:E"], "APP", "E"),
    (["*:S"], "APP", ""),
    (["*:N"], "APP", ""),
    (["*:W", "PRESSURE:D"], "PRESSURE", "EWID"),
    (["*:W", "PRESSURE:D"], "APP", "EW"),
    (["*:S", "PRESSURE:D"], "PRESSURE", "EWID"),
    (["*:S", "PRESSURE:D"], "APP", ""),
    (["PRESSURE:D"], "APP", "EWIDV"),  # adb: without *:, everything else stays visible
    (["PRESSURE:D"], "PRESSURE", "EWID"),
    (["PRESSURE:S"], "PRESSURE", ""),
    (["PRESSURE:N"], "PRESSURE", ""),
    (["PRESSURE"], "PRESSURE", "EWIDV"),  # adb: bare tag = TAG:V
    (["*:S", "PRESSURE"], "PRESSURE", "EWIDV"),
    (["PRESSURE:D", "*:S"], "PRESSURE", "EWID"),  # exact tag wins whatever the order
    (["PRESSURE:D", "PRESSURE:E"], "PRESSURE", "E"),  # same tag twice: the last one
    (["*:W", "*:I"], "APP", "EWI"),  # default twice: the last one
    (["pressure:D", "*:S"], "PRESSURE", ""),  # tags are case-sensitive (like adb)
    (["*:w", "PRESSURE:d"], "PRESSURE", "EWID"),  # levels are not
    (["boot.esp32c3:W", "*:S"], "boot.esp32c3", "EW"),  # ESP-IDF tags may contain dots
]


@pytest.mark.parametrize(("specs", "tag", "expected"), TABLE)
def test_spec_table(specs, tag, expected):
    assert shown(specs, tag) == expected


@pytest.mark.parametrize(("specs", "tag", "expected"), TABLE)
def test_esp_idf_lines_follow_the_same_rules(specs, tag, expected):
    line_filter = LogFilter.parse(specs)
    assert "".join(lv for lv in "EWIDV" if line_filter.accepts(idf(tag, lv))) == expected


@pytest.mark.parametrize(
    "line",
    [
        RawLine(text="ESP-ROM:esp32c3-api1-20210207"),
        CommandReply(id="1", fields=(("a", "1"),)),
        CommandEnd(id="1", code="ok"),
        DumpBegin(id="1", filename="log.0", size=1),
        DumpEnd(id="1", crc32=0),
        InvalidLine(reason=InvalidReason.TORN, raw=b"0000:00"),
    ],
)
def test_lines_without_tag_always_pass(line):
    assert LogFilter.parse(["*:S"]).accepts(line)


def test_no_specs_is_trivial():
    assert LogFilter.parse([]).is_trivial
    assert LogFilter.parse(["*:V", "APP:V"]).is_trivial
    assert not LogFilter.parse(["APP:I"]).is_trivial
    assert not LogFilter.parse(["*:S"]).is_trivial


def test_describe_normalizes():
    assert LogFilter.parse(["*:w", "PRESSURE:d", "NOISY:N"]).describe() == (
        "*:W PRESSURE:D NOISY:S"
    )
    assert LogFilter.parse([]).describe() == "*:V"
    assert LogFilter.parse(["APP:D", "LOGCAT:W", "APP:E"]).describe() == "LOGCAT:W APP:E"


@pytest.mark.parametrize(
    ("spec", "message"),
    [
        ("PRESSURE:F", r"no F \(fatal\) level; use E"),
        ("PRESSURE:X", "level must be one of V D I W E S N"),
        ("PRESSURE:", "level must be one of"),
        (":W", "missing tag"),
        ("", "empty"),
        ("   ", "empty"),
        (":", "missing tag"),
        ("APP::W", "cannot contain"),
        ("a:b:W", "cannot contain"),
        ("APP:ſ", "level must be one of"),  # long s: upper() is S, still rejected
        ("PRESSURE:WW", "level must be one of"),
    ],
)
def test_invalid_specs_explain_themselves(spec, message):
    with pytest.raises(FilterSpecError, match=message):
        LogFilter.parse([spec])


def test_one_argument_may_hold_several_specs():
    # Copied from the "-- filter: ... --" notice, a PowerShell variable, or the TUI input box.
    together = LogFilter.parse(["*:S HEARTBEAT:I"])
    assert together == LogFilter.parse(["*:S", "HEARTBEAT:I"])
    assert LogFilter.parse("  *:W   PRESSURE:d ") == LogFilter.parse(["*:W", "PRESSURE:D"])


def test_a_single_string_is_not_split_into_characters():
    assert LogFilter.parse("APP:S").tags == {"APP": 0}


@pytest.mark.parametrize(("specs", "tag", "expected"), TABLE)
def test_describe_round_trips(specs, tag, expected):
    line_filter = LogFilter.parse(specs)
    assert LogFilter.parse(line_filter.describe()) == line_filter


def test_bare_wildcard_means_show_everything():
    assert LogFilter.parse(["*:S", "*"]).is_trivial


def test_filter_is_immutable_and_hashable():
    line_filter = LogFilter.parse(["*:W", "APP:D"])
    with pytest.raises(TypeError):
        line_filter.tags["APP"] = 0  # type: ignore[index]
    assert hash(line_filter) == hash(LogFilter.parse(["APP:D", "*:W"]))
    assert len({line_filter, LogFilter.parse(["APP:D", "*:W"])}) == 1
    caller = {"APP": 3}
    built = LogFilter(tags=caller)
    caller["APP"] = 0
    assert built.tags == {"APP": 3}  # never aliases the caller's dict


@pytest.mark.parametrize("bad", [{"default": 7}, {"tags": {"APP": -1}}])
def test_thresholds_are_validated(bad):
    with pytest.raises(ValueError, match="out of range"):
        LogFilter(**bad)


def test_unknown_level_letter_is_shown_rather_than_hidden():
    assert LogFilter.parse(["*:S"]).accepts(log("APP", "X"))


def test_equal_filters_hash_equal_whatever_the_spec_order():
    first = LogFilter.parse(["A:W", "B:E"])
    second = LogFilter.parse(["B:E", "A:W"])
    assert first == second
    assert hash(first) == hash(second)
    assert first.describe() != second.describe()  # describe() keeps the order given
