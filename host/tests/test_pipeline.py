from datetime import datetime
from pathlib import Path

from esp_logcat.events import Notice, NoticeKind, Received
from esp_logcat.parser import IdfLine, InvalidLine, InvalidReason, LogLine, RawLine
from esp_logcat.pipeline import events
from esp_logcat.stats import StreamStats

FIXTURES = Path(__file__).parent / "fixtures"
T0 = datetime(2026, 11, 7, 10, 0, 0)


def run(chunks: list[bytes | Notice]) -> tuple[list[object], StreamStats]:
    stats = StreamStats()
    out = list(events(chunks, stats, clock=lambda: T0))
    return out, stats


def test_board_capture_through_the_pipeline():
    data = (FIXTURES / "boot_esp32c3_rev4.log").read_bytes()
    out, stats = run([data[i : i + 37] for i in range(0, len(data), 37)])
    lines = [event.line for event in out if isinstance(event, Received)]
    assert len(lines) == 63
    assert {type(line) for line in lines} == {LogLine, IdfLine, RawLine}
    assert all(event.pc_time == T0 for event in out if isinstance(event, Received))
    assert stats.kinds["IdfLine"] == 52


def test_gap_notice_comes_before_the_line():
    out, _ = run([b"0000:000001 0000000010 I T: a\r\n0000:000004 0000000011 I T: b\r\n"])
    assert isinstance(out[0], Received)
    assert isinstance(out[1], Notice) and out[1].kind is NoticeKind.GAP
    assert isinstance(out[2], Received) and out[2].line.seq == 4


def test_disconnect_flushes_the_tail_as_torn_and_resets_continuity():
    connected = Notice(NoticeKind.CONNECTED, "connected to COM6")
    lost = Notice(NoticeKind.DISCONNECTED, "COM6: connection lost")
    out, stats = run(
        [
            connected,
            b"0000:000001 0000000010 I T: a\n0000:00",
            lost,
            connected,
            b"0000:000090 0000000900 I T: b\n",
        ]
    )
    kinds = [
        type(event.line).__name__ if isinstance(event, Received) else event.kind for event in out
    ]
    assert kinds == [
        NoticeKind.CONNECTED,
        "LogLine",
        "InvalidLine",
        NoticeKind.DISCONNECTED,
        NoticeKind.CONNECTED,
        "LogLine",
    ]
    torn = out[2].line
    assert isinstance(torn, InvalidLine) and torn.reason is InvalidReason.TORN
    assert stats.lost == 0
    assert stats.reconnects == 1


def test_stale_fragment_does_not_glue_onto_the_first_live_line():
    # Seen on the board: the 64-byte FIFO fragment + the next live line formed a valid-looking
    # LogLine with seq 34 and a bogus message, and a false "17 lines lost".
    # The FIFO bytes leave the device only with the next write: one chunk, as captured.
    out, stats = run(
        [
            Notice(NoticeKind.CONNECTED, "connected to COM6"),
            b"0000:000034 0000029110 I HEARTBEAT: tick 29, capture 12566 cycle"
            b"0000:000051 0000046160 I HEARTBEAT: tick 46\r\n",
            b"0000:000052 0000047160 I HEARTBEAT: tick 47\r\n",
        ]
    )
    lines = [event.line for event in out if isinstance(event, Received)]
    assert lines[0] == InvalidLine(
        reason=InvalidReason.TORN,
        raw=b"0000:000034 0000029110 I HEARTBEAT: tick 29, capture 12566 cycle",
    )
    assert [line.seq for line in lines[1:]] == [51, 52]
    assert stats.lost == 0
    assert not any(isinstance(e, Notice) and e.kind is NoticeKind.GAP for e in out)


def test_stale_fragment_glued_to_the_rom_banner_after_a_reset():
    out, _ = run(
        [
            Notice(NoticeKind.CONNECTED, "connected to COM6"),
            b"0000:000032 0000027070 I HEARTBEAT: tick 27, capture 12566 cycle"
            b"ESP-ROM:esp32c3-api1-20210207\r\nBuild:Feb  7 2021\r\n",
        ]
    )
    lines = [event.line for event in out if isinstance(event, Received)]
    assert isinstance(lines[0], InvalidLine) and lines[0].reason is InvalidReason.TORN
    assert lines[1] == RawLine(text="ESP-ROM:esp32c3-api1-20210207")
    assert lines[2] == RawLine(text="Build:Feb  7 2021")


def test_live_tail_after_a_split_counts_real_gaps():
    out, stats = run(
        [
            Notice(NoticeKind.CONNECTED, "connected to COM6"),
            b"0000:000034 0000029"  # FIFO head
            b"0000:000051 0000046160 I T: a\r\n"  # live, glued to it
            b"0000:000060 0000047160 I T: b\r\n",  # 8 lines really lost on the device
        ]
    )
    assert [e.kind for e in out if isinstance(e, Notice)] == [
        NoticeKind.CONNECTED,
        NoticeKind.GAP,
    ]
    assert stats.lost == 8 and stats.missed == 0


def test_lines_past_the_fifo_window_are_not_split():
    glued = b"0000:000001 0000000010 I T: see 0000:000002 0000000011 I T: x\n"
    first = b"r" * 63 + b"\n"  # fills the 64 bytes the FIFO can hold
    out, _ = run([Notice(NoticeKind.CONNECTED, "c"), first, glued])
    lines = [event.line for event in out if isinstance(event, Received)]
    assert len(lines) == 2 and isinstance(lines[1], LogLine)
    assert lines[1].msg == "see 0000:000002 0000000011 I T: x"


def test_short_lines_and_a_head_inside_the_fifo_window():
    # The FIFO keeps the first 64 bytes written: a whole short line plus the head of the next
    # one, glued to the first live line. Both are "maybe stale"; the jump to the live line is
    # missed before the connection, not lost.
    out, stats = run(
        [
            Notice(NoticeKind.CONNECTED, "connected to COM6"),
            b"0000:000034 0000029110 I T: tick 29\r\n"  # 39 bytes on the wire
            b"0000:000035 0000029"  # head of 35, the rest dropped
            b"0000:000051 0000046160 I T: tick 46\r\n"
            b"0000:000052 0000047160 I T: tick 47\r\n",
        ]
    )
    lines = [event.line for event in out if isinstance(event, Received)]
    assert [type(line).__name__ for line in lines] == [
        "LogLine",
        "InvalidLine",
        "LogLine",
        "LogLine",
    ]
    assert [line.seq for line in lines if isinstance(line, LogLine)] == [34, 51, 52]
    notices = [event.kind for event in out if isinstance(event, Notice)]
    assert NoticeKind.GAP not in notices and NoticeKind.MISSED in notices
    assert stats.lost == 0 and stats.missed == 16


def test_split_again_after_a_reconnect():
    glued = b"0000:000034 0000029110 I T: head" + b"0000:000051 0000046160 I T: live\n"
    out, _ = run(
        [
            Notice(NoticeKind.CONNECTED, "c"),
            b"first\n",
            Notice(NoticeKind.DISCONNECTED, "lost"),
            Notice(NoticeKind.CONNECTED, "c"),
            glued,
        ]
    )
    lines = [event.line for event in out if isinstance(event, Received)]
    assert isinstance(lines[1], InvalidLine)
    assert isinstance(lines[2], LogLine) and lines[2].seq == 51


def test_overflow_notice_and_count():
    out, stats = run([b"z" * 3000 + b"\nok\n"])
    assert [e.kind for e in out if isinstance(e, Notice)] == [NoticeKind.OVERFLOW]
    assert stats.overflows == 1
    raw = [e.line for e in out if isinstance(e, Received)]
    assert raw == [RawLine(text="z" * 1024), RawLine(text="ok")]


def test_tail_at_end_of_input_is_torn():
    out, _ = run([b"0000:000001 0000000010 I T: a\n0000:0000"])
    assert isinstance(out[-1], Received)
    assert out[-1].line == InvalidLine(reason=InvalidReason.TORN, raw=b"0000:0000")


# ---------------------------------------------------------------- display filter (visible)


def test_hidden_lines_still_count_for_gaps():
    from esp_logcat.filters import LogFilter
    from esp_logcat.pipeline import visible

    chunks = [
        b"0000:000003 0000001000 I HEARTBEAT: a\n"
        b"0000:000004 0000001001 I APP: hidden in between\n"
        b"0000:000005 0000002000 I HEARTBEAT: b\n"
    ]
    stats = StreamStats()
    out = list(visible(events(chunks, stats), stats, LogFilter.parse("*:S HEARTBEAT:I")))
    assert out[0] == Notice(NoticeKind.FILTER, "filter: *:S HEARTBEAT:I")
    shown = [e.line.seq for e in out if isinstance(e, Received)]
    assert shown == [3, 5]
    assert not any(isinstance(e, Notice) and e.kind is NoticeKind.GAP for e in out)
    assert stats.hidden == 1
    assert stats.summary().startswith("3 lines, 3 log")
    assert stats.summary().endswith("| 1 hidden by filter")


def test_trivial_filter_changes_nothing():
    from esp_logcat.filters import LogFilter
    from esp_logcat.pipeline import visible

    data = [b"0000:000001 0000000001 I APP: x\n", b"ROM text\n"]
    for line_filter in (None, LogFilter.parse("*:V")):
        stats = StreamStats()
        filtered = list(visible(events(data, stats, clock=lambda: T0), stats, line_filter))
        plain = list(events(data, StreamStats(), clock=lambda: T0))
        assert filtered == plain  # no FILTER notice, nothing dropped
        assert not stats.filtering
        assert "hidden" not in stats.summary()


def test_lenient_is_passed_to_the_parser():
    data = [b"0000:000001 0000000010 I APP:\n"]
    strict = [e.line for e in events(data, StreamStats()) if isinstance(e, Received)]
    lenient = [e.line for e in events(data, StreamStats(), lenient=True) if isinstance(e, Received)]
    assert isinstance(strict[0], InvalidLine)
    assert isinstance(lenient[0], LogLine) and lenient[0].msg == ""


def test_saved_file_drops_the_pc_time_column_only_when_asked():
    data = [b"10:00:00.001 0000:000001 0000000010 I APP: x\n"]
    saved = [
        e.line for e in events(data, StreamStats(), saved_file=True) if isinstance(e, Received)
    ]
    live = [e.line for e in events(data, StreamStats()) if isinstance(e, Received)]
    assert isinstance(saved[0], LogLine)
    assert isinstance(live[0], RawLine)  # a live device line is never rewritten


# ------------------------------------------------------------------ dump blocks (the shell, #30)


def test_framer_turns_a_block_into_one_event_outside_the_statistics():
    import zlib

    import pytest

    from esp_logcat.events import ReceivedBlock
    from esp_logcat.framing import Framer

    content = b"0000:000009 0000000090 I OLD: from the tail\n"
    stream = (
        b"0000:000001 0000000010 I T: a\n"
        + f"#BEGIN 4 ram {len(content)}\n".encode()
        + content
        + f"#END 4 {zlib.crc32(content):08x}\n".encode()
        + b"#. 4 ok\n"
        + b"0000:000002 0000000011 I T: b\n"
    )
    stats = StreamStats()
    out = list(events([stream], stats, clock=lambda: T0, framer=Framer(lambda ident: ident == "4")))
    blocks = [event for event in out if isinstance(event, ReceivedBlock)]
    assert len(blocks) == 1 and blocks[0].block.content == content and blocks[0].pc_time == T0
    # seq 9 inside the block would be a "reorder" and a "gap" if it counted: it does not.
    assert not [event for event in out if isinstance(event, Notice)]
    assert stats.kinds["LogLine"] == 2 and stats.kinds["CommandEnd"] == 1

    with pytest.raises(ValueError):
        list(events([b""], StreamStats(), framer=Framer(lambda ident: True), saved_file=True))
