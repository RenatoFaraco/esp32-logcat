import pytest

from esp_logcat.events import NoticeKind
from esp_logcat.parser import IdfLine, InvalidLine, InvalidReason, LogLine, RawLine
from esp_logcat.stats import REORDER_WINDOW, StreamStats


def log(seq: int, uptime: int = 1000, boot: int = 0) -> LogLine:
    return LogLine(
        boot=boot, seq=seq, uptime_ms=uptime, level="I", tag="T", msg="m", truncated=False
    )


def feed(stats: StreamStats, *lines: LogLine) -> list[NoticeKind]:
    return [notice.kind for line in lines for notice in stats.observe(line)]


def test_contiguous_sequence_has_no_notices():
    stats = StreamStats()
    assert feed(stats, *(log(seq, 1000 + seq) for seq in range(10))) == []
    assert stats.lost == stats.reordered == stats.restarts == 0


def test_gap_counts_missing_lines():
    stats = StreamStats()
    notices = [n for line in (log(5), log(6), log(10)) for n in stats.observe(line)]
    assert [n.kind for n in notices] == [NoticeKind.GAP]
    assert notices[0].text == "3 lines lost before seq 000010"
    assert stats.lost == 3
    assert feed(stats, log(11)) == []  # continues from the new position


def test_single_line_gap_wording():
    stats = StreamStats()
    notices = [n for line in (log(1), log(3)) for n in stats.observe(line)]
    assert notices[0].text == "1 line lost before seq 000003"


def test_seq_wrap_is_contiguous():
    stats = StreamStats()
    assert feed(stats, log(999_998), log(999_999), log(0), log(1)) == []


def test_gap_across_the_wrap():
    stats = StreamStats()
    assert feed(stats, log(999_999), log(2)) == [NoticeKind.GAP]
    assert stats.lost == 2


def test_reorder_within_the_queue_depth_undoes_the_loss():
    stats = StreamStats()
    notices = [n for line in (log(10), log(12), log(11), log(13)) for n in stats.observe(line)]
    assert [n.kind for n in notices] == [NoticeKind.GAP, NoticeKind.REORDER]
    assert notices[1].text == "seq 000011 arrived after 000012 (not lost after all)"
    assert stats.reordered == 1
    assert stats.lost == 0  # 11 was reported lost at 12, then arrived


def test_reorder_of_a_seq_never_reported_lost():
    stats = StreamStats()
    notices = [n for line in (log(10), log(11), log(10)) for n in stats.observe(line)]
    assert notices[0].text == "seq 000010 arrived after 000011"
    assert stats.lost == 0


def test_only_recent_losses_can_be_undone():
    stats = StreamStats()
    feed(stats, log(0), log(101))  # 100 lost; only the last 32 (69..100) are remembered
    assert stats.lost == 100
    feed(stats, log(100))
    assert stats.lost == 99
    feed(stats, log(70))
    assert stats.lost == 98


def test_reorder_window_edge():
    stats = StreamStats()
    feed(stats, log(100))
    assert feed(stats, log(100 - REORDER_WINDOW)) == [NoticeKind.REORDER]


def test_seq_far_behind_with_uptime_forward_is_a_corrupted_seq():
    # A bad digit (no checksum, ADR-007): 000137 read as 000037.
    stats = StreamStats()
    notices = [
        n
        for line in (log(136, 5_000), log(37, 5_010), log(138, 5_020))
        for n in stats.observe(line)
    ]
    # 137 arrived as "37": reported once, and 136 -> 138 then shows it as 1 line lost (it was
    # not received intact); never the ~1e6 lines a plain gap computation would give.
    assert [n.kind for n in notices] == [NoticeKind.SEQ_BACK, NoticeKind.GAP]
    assert stats.seq_back == 1
    assert stats.lost == 1


def test_seq_far_behind_with_uptime_back_a_little_is_a_restart():
    stats = StreamStats()
    assert feed(stats, log(49, 400), log(0, 300)) == [NoticeKind.RESTART]
    assert stats.lost == 0


def test_rom_banner_marks_a_restart_even_early_in_the_boot():
    # Boot loop seen on the CH340K: the previous boot reached seq 9 at 400 ms, the new one
    # starts at seq 0, 300 ms. Without the banner this looks like a reorder.
    stats = StreamStats()
    feed(stats, *(log(seq, 300 + 10 * seq) for seq in range(10)))
    stats.observe(RawLine(text="ESP-ROM:esp32c3-api1-20210207"))
    assert feed(stats, log(0, 300), log(1, 310)) == [NoticeKind.RESTART]
    assert stats.reordered == 0


def test_duplicate_seq_is_reorder_not_gap():
    stats = StreamStats()
    assert feed(stats, log(7), log(7)) == [NoticeKind.REORDER]


def test_restart_when_seq_and_uptime_go_back():
    stats = StreamStats()
    assert feed(stats, log(500, 60_000), log(0, 250), log(1, 260)) == [NoticeKind.RESTART]
    assert stats.restarts == 1
    assert stats.lost == 0


def test_restart_early_in_the_boot_is_not_a_reorder():
    stats = StreamStats()
    assert feed(stats, log(5, 3_000), log(0, 250)) == [NoticeKind.RESTART]


def test_boot_change_is_a_restart():
    stats = StreamStats()
    assert feed(stats, log(40, 5_000, boot=1), log(41, 5_001, boot=2)) == [NoticeKind.RESTART]


def test_uptime_back_with_contiguous_seq():
    stats = StreamStats()
    notices = [n for line in (log(1, 90_000), log(2, 10_000)) for n in stats.observe(line)]
    assert [n.kind for n in notices] == [NoticeKind.UPTIME_BACK]
    assert "80000 ms" in notices[0].text
    assert stats.uptime_back == 1


def test_small_uptime_step_back_is_tolerated():
    stats = StreamStats()
    assert feed(stats, log(1, 5_000), log(2, 4_500)) == []


def test_uptime_wrap_is_not_suspicious():
    stats = StreamStats()
    assert feed(stats, log(1, 0xFFFF_FFF0), log(2, 5)) == []


def test_reconnect_resets_continuity():
    stats = StreamStats()
    stats.connected()
    feed(stats, log(1))
    stats.connected()
    assert feed(stats, log(50)) == []
    assert stats.reconnects == 1
    assert stats.lost == 0


def test_reset_while_disconnected_is_a_restart():
    # Reset button on the native USB: the port drops and the ROM banner is gone before the
    # host reopens it. The board seen on the screen: seq 110 at 4030 ms, then seq 107 at 1030.
    stats = StreamStats()
    stats.connected()
    feed(stats, log(109, uptime=3030), log(110, uptime=4030))
    stats.connected()
    assert feed(stats, log(107, uptime=1030), log(108, uptime=1034)) == [NoticeKind.RESTART]
    assert stats.restarts == 1
    assert stats.lost == 0


@pytest.mark.parametrize(
    "first",
    [
        log(5, uptime=900_000),  # seq went back, uptime forward: the new boot ran longer
        log(150, uptime=2_000),  # seq ahead, uptime back: a boot that logged more early
        log(111, uptime=9_000, boot=1),  # boot counter changed (M4)
        log(110, uptime=9_000),  # the same seq again: the new boot reached it
    ],
)
def test_other_restarts_across_a_reconnect(first):
    stats = StreamStats()
    stats.connected()
    feed(stats, log(110, uptime=4030))
    stats.connected()
    assert feed(stats, first) == [NoticeKind.RESTART]


def test_cable_out_and_back_is_not_a_restart():
    stats = StreamStats()
    stats.connected()
    feed(stats, log(110, uptime=4030))
    stats.connected()
    assert feed(stats, log(140, uptime=34_030), log(141, uptime=35_030)) == []
    assert stats.restarts == 0 and stats.lost == 0  # not received != lost (no reader)


def test_only_the_first_line_after_a_reconnect_is_compared():
    stats = StreamStats()
    stats.connected()
    feed(stats, log(110, uptime=4030))
    stats.connected()
    stats.connected()  # dropped again before any line: still compared with seq 110
    assert feed(stats, log(3, uptime=27)) == [NoticeKind.RESTART]
    assert feed(stats, log(4, uptime=27)) == []


def test_first_jump_after_connecting_is_missed_not_lost():
    stats = StreamStats()
    stats.connected()
    notices = stats.observe(log(34), maybe_stale=True)  # sat in the FIFO
    notices += [n for line in (log(51), log(60)) for n in stats.observe(line)]
    assert [n.kind for n in notices] == [NoticeKind.MISSED, NoticeKind.GAP]
    assert (
        notices[0].text == "16 lines written before the connection not received (up to seq 000051)"
    )
    assert stats.missed == 16
    assert stats.lost == 8


def test_contiguous_start_clears_the_connection_grace():
    stats = StreamStats()
    stats.connected()
    assert feed(stats, log(1), log(2), log(9)) == [NoticeKind.GAP]
    assert stats.missed == 0


def test_without_a_connection_every_jump_is_a_gap():
    stats = StreamStats()  # replay of a file: no connection events
    assert feed(stats, log(1), log(5)) == [NoticeKind.GAP]


def test_other_kinds_are_counted_but_ignored_for_continuity():
    stats = StreamStats()
    feed(stats, log(1))
    stats.observe(IdfLine(level="I", timestamp_ms=1, tag="T", msg="m"))
    stats.observe(RawLine(text="rom"))
    stats.observe(InvalidLine(reason=InvalidReason.TORN, raw=b"00"))
    stats.observe(InvalidLine(reason=InvalidReason.GARBAGE, raw=b"x"))
    stats.observe(InvalidLine(reason=InvalidReason.TORN, raw=b"01"))
    assert feed(stats, log(2)) == []
    assert stats.kinds["IdfLine"] == 1 and stats.kinds["InvalidLine"] == 3
    assert stats.invalid == {"torn": 2, "garbage": 1}


def test_summary():
    stats = StreamStats()
    feed(stats, log(1), log(3))
    stats.observe(InvalidLine(reason=InvalidReason.TORN, raw=b"00"))
    summary = stats.summary()
    assert summary.startswith("3 lines, 2 log, 0 esp-idf, 0 raw, 1 invalid (torn 1)")
    assert "lost 1" in summary


@pytest.mark.parametrize("calls", [1, 3])
def test_first_connection_is_not_a_reconnect(calls):
    stats = StreamStats()
    for _ in range(calls):
        stats.connected()
    assert stats.reconnects == calls - 1
