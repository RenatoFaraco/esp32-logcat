import io
from datetime import datetime, timedelta

import pytest

from esp_logcat import cli
from esp_logcat.cli import main, run_stream
from esp_logcat.events import Notice, NoticeKind
from esp_logcat.filters import LogFilter
from esp_logcat.parser import InvalidLine
from esp_logcat.pipeline import events
from esp_logcat.session import (
    HEADER,
    SessionWriter,
    format_time,
    parse_metadata,
    parse_time,
    split_timed,
)
from esp_logcat.stats import StreamStats
from esp_logcat.transport import FileTransport

T0 = datetime(2026, 11, 8, 10, 15, 2, 120000)

# A live session with everything that makes a replay hard: the native port's stale FIFO head
# glued to the first live line, a dropped connection with a torn tail, a reconnection, a gap
# on the device, a line cut over 1024 bytes and a burst report.
LIVE = [
    Notice(NoticeKind.WAITING, "waiting for COM6: port busy (is a serial monitor open?)"),
    Notice(NoticeKind.CONNECTED, "connected to COM6"),
    b"0000:000034 0000029110 I HEARTBEAT: tick 29, capture 1"
    b"0000:000051 0000046160 I HEARTBEAT: tick 46\r\n",
    b"0000:000052 0000047160 W APP: careful\r\n0000:000053 0000048160 I APP: half",
    Notice(NoticeKind.DISCONNECTED, "COM6: connection lost (ClearCommError failed)"),
    Notice(NoticeKind.CONNECTED, "connected to COM7 (was COM6)"),
    b"0000:000090 0000085160 E APP: back\n0000:000095 0000090160 I APP: after a gap\n",
    b"x" * 1500 + b"\n",
    b"0000:000096 0000091160 W LOGCAT: dropped 4 lines: log queue full\n",
]


def ticking_clock() -> callable:
    moments = (T0 + timedelta(milliseconds=7 * i) for i in range(10_000))
    return lambda: next(moments)


def run_live(**kwargs: object) -> tuple[str, str, bytes]:
    saved = io.BytesIO()
    saved.close = lambda: None  # keep the bytes readable after the writer "closes"
    recorder = SessionWriter(saved, clock=ticking_clock())
    out, err = io.StringIO(), io.StringIO()
    clock = ticking_clock()
    original = cli.events

    def events_with_clock(*args: object, **kw: object):
        return original(*args, clock=clock, **kw)

    cli.events = events_with_clock
    try:
        run_stream(iter(LIVE), out, err, recorder=recorder, **kwargs)  # type: ignore[arg-type]
    finally:
        cli.events = original
    return out.getvalue(), err.getvalue(), saved.getvalue()


def run_replay(saved: bytes, **kwargs: object) -> tuple[str, str, int]:
    transport = FileTransport(io.BytesIO(saved), "session.log")
    out, err = io.StringIO(), io.StringIO()
    code = run_stream(transport.chunks(), out, err, saved_file=True, **kwargs)  # type: ignore[arg-type]
    return out.getvalue(), err.getvalue(), code


# ---------------------------------------------------------------- the round trip (Done when)


def test_saved_session_replays_identically():
    live_out, live_err, saved = run_live(show_time=True)
    replay_out, replay_err, code = run_replay(saved, show_time=True)
    assert code == 0
    assert replay_out == live_out  # every line, its PC time, its colors' text
    replay_lines = replay_err.splitlines()
    assert replay_lines[0] == "-- replaying session.log --"
    assert "\n".join(replay_lines[1:]) == live_err.rstrip("\n")  # notices, gaps, summary


def test_round_trip_keeps_what_plain_redirection_loses():
    live_out, live_err, saved = run_live(show_time=True)
    summary = live_err.splitlines()[-1]
    assert "reconnects 1" in summary and "overflows 1" in summary
    assert "missed before connecting 4" in summary  # 90 -> 95 right after reconnecting
    assert "!! torn:" in live_out  # the tail cut by the disconnection
    # The torn tail and the 1024-byte cut. The FIFO head is saved glued to its line, as it
    # arrived: the replay splits it again, like the live stream did.
    assert saved.count(b" raw ") == 2
    assert b"capture 10000:000051" in saved


def test_saving_ignores_the_display_filter():
    live_out, _, saved = run_live(line_filter=LogFilter.parse("*:E"))
    assert "W APP: careful" not in live_out  # the screen was filtered...
    replay_out, _, _ = run_replay(saved)
    assert "W APP: careful" in replay_out  # ...the file was not
    assert "I HEARTBEAT: tick 46" in replay_out


# ---------------------------------------------------------------- writer and reader


def test_writer_records_lines_partials_and_transport_notices_only():
    buffer = io.BytesIO()
    writer = SessionWriter(buffer, clock=lambda: T0)
    writer.line(b"0000:000001 0000000001 I APP: x\n", T0)
    writer.line(b"0000:00", None)
    writer.notice(Notice(NoticeKind.CONNECTED, "connected to COM6"))
    writer.notice(Notice(NoticeKind.GAP, "3 lines lost before seq 000010"))  # computed: skipped
    writer.notice(Notice(NoticeKind.FILTER, "filter: *:W"))  # display only: skipped
    assert (
        buffer.getvalue()
        == (
            HEADER + b"2026-11-08T10:15:02.120 0000:000001 0000000001 I APP: x\n"
            b"# - raw 303030303a3030\n"  # no newline and no known time: a raw record
            b"# 2026-11-08T10:15:02.120 notice connected connected to COM6\n"
        )
    )


def test_writer_flushes_every_record():
    class Counting(io.BytesIO):
        flushes = 0

        def flush(self) -> None:
            Counting.flushes += 1

    writer = SessionWriter(Counting(), clock=lambda: T0)
    writer.line(b"a\n", None)
    writer.notice(Notice(NoticeKind.WAITING, "w"))
    assert Counting.flushes == 3  # header + 2 records: Ctrl+C keeps them


def test_time_format_keeps_the_date_and_milliseconds():
    assert format_time(T0) == "2026-11-08T10:15:02.120"
    assert parse_time("2026-11-08T10:15:02.120") == T0
    assert parse_time("garbage") is None


def test_split_timed():
    assert split_timed(b"2026-11-08T10:15:02.120 ESP-ROM:x\n") == (T0, b"ESP-ROM:x\n")
    assert split_timed(b"10:15:02.120 ESP-ROM:x\n") is None
    assert split_timed(b"ESP-ROM:x\n") is None


@pytest.mark.parametrize(
    "raw",
    [
        b"# esp-logcat session v1\n",
        b"# a comment\n",
        b"# 2026-11-08T10:15:02.120 notice no_such_kind text\n",
        b"# 2026-11-08T10:15:02.120 raw zz\n",
        b"# 2026-11-08T10:15:02.120 future_record x\n",
    ],
)
def test_unknown_or_damaged_metadata_is_ignored(raw):
    assert parse_metadata(raw) is None


def test_replay_of_a_damaged_session_does_not_crash():
    saved = (
        HEADER + b"# 2026-11-08T10:15:02.120 raw zz\n"
        b"# 2026-11-08T10:15:02.120 notice bogus x\n"
        b"2026-11-08T10:15:02.120 0000:000001 0000000001 I APP: ok\n"
    )
    out, _, code = run_replay(saved, show_time=True)
    assert code == 0
    assert out == "10:15:02.120 0000:000001 0000000001 I APP: ok\n"


def test_replay_time_of_a_plain_log_shows_no_column():
    out, _, _ = run_replay(b"0000:000001 0000000001 I APP: plain\n", show_time=True)
    assert out == "0000:000001 0000000001 I APP: plain\n"


# ---------------------------------------------------------------- CLI


def test_cli_save_and_replay(tmp_path, monkeypatch, capsys):
    class FakeTransport:
        def __init__(self, port: str, baudrate: int, *, reset: bool) -> None:
            pass

        def chunks(self):
            yield from LIVE

        def close(self) -> None:
            pass

    monkeypatch.setattr(cli, "SerialTransport", FakeTransport)
    session = tmp_path / "s.log"
    assert main(["stream", "--port", "COM6", "--save", str(session), "*:E"]) == 0
    capsys.readouterr()
    assert session.read_bytes().startswith(HEADER)
    assert main(["replay", str(session), "--time"]) == 0
    out = capsys.readouterr().out
    assert "W APP: careful" in out and out.split(" ")[0].count(":") == 2


def test_cli_save_to_an_unwritable_path(tmp_path, capsys):
    with pytest.raises(SystemExit) as exc:
        main(["replay", "-", "--save", str(tmp_path)])  # a directory
    assert exc.value.code == 2
    assert "cannot write" in capsys.readouterr().err


# ---------------------------------------------------------------- review findings (#21)


def saved_session(chunks: list) -> bytes:
    saved = io.BytesIO()
    saved.close = lambda: None
    recorder = SessionWriter(saved, clock=ticking_clock())
    list(events(iter(chunks), StreamStats(), clock=ticking_clock(), recorder=recorder))
    return saved.getvalue()


def comparable(stream: list) -> list:
    # FILTER/SOURCE are frontend notices; pc_time is compared at the file's resolution (ms).
    out = []
    for event in stream:
        if isinstance(event, Notice):
            if event.kind is not NoticeKind.SOURCE:
                out.append(("notice", event.kind, event.text))
        else:
            stamp = event.pc_time.replace(microsecond=event.pc_time.microsecond // 1000 * 1000)
            out.append(("line", event.line, stamp))
    return out


def test_event_sequence_round_trips_even_read_in_tiny_pieces():
    live = list(events(iter(LIVE), StreamStats(), clock=ticking_clock()))
    data = saved_session(LIVE)
    pieces = [data[i : i + 7] for i in range(0, len(data), 7)]  # records split everywhere
    replayed = list(events(iter(pieces), StreamStats(), saved_file=True))
    assert comparable(replayed) == comparable(live)  # same events, same order


def test_torn_tail_comes_before_the_disconnection_notice():
    data = saved_session(LIVE)
    torn = data.index(b"raw 303030303a303030303533")
    disconnected = data.index(b"notice disconnected")
    assert torn < disconnected


def test_lone_cr_before_the_newline_survives_the_round_trip():
    chunks = [
        Notice(NoticeKind.CONNECTED, "c"),
        b"x" * 70 + b"\n",
        b"0000:000001 0000000001 I APP: m\r\r\n",
    ]
    live = list(events(iter(chunks), StreamStats(), clock=ticking_clock()))
    replayed = list(events(iter([saved_session(chunks)]), StreamStats(), saved_file=True))
    assert comparable(replayed) == comparable(live)
    assert isinstance(comparable(live)[-1][1], InvalidLine)  # still garbage, not "fixed"


def test_hash_lines_of_a_plain_log_stay_device_lines():
    out, _, _ = run_replay(b"# printf from the app\n0000:000001 0000000001 I APP: x\n")
    assert out.splitlines()[0] == "# printf from the app"


def test_replay_save_chain_is_stable_for_long_lines():
    plain = b"y" * 3000 + b"\n0000:000001 0000000001 I APP: after\n"
    first = io.BytesIO()
    first.close = lambda: None
    recorder = SessionWriter(first)
    out1, err1, _ = run_replay(plain, recorder=recorder)
    out2, err2, _ = run_replay(first.getvalue())
    assert out2 == out1
    assert err2.splitlines()[1:] == err1.splitlines()[1:]
    assert "over 1024 bytes" in err1  # cut like the live splitter would
    assert b"# - raw " in first.getvalue()  # unknown time kept unknown, not invented


def test_write_error_stops_saving_but_not_the_stream():
    class Full(io.BytesIO):
        writes = 0

        def write(self, data: bytes) -> int:
            Full.writes += 1
            if Full.writes > 2:
                raise OSError(28, "No space left on device")
            return super().write(data)

    recorder = SessionWriter(Full())
    out, err, code = run_replay(b"0000:000001 0000000001 I APP: a\n" * 5, recorder=recorder)
    assert code == 0
    assert out.count("I APP: a") == 5
    assert err.count("save stopped: No space left on device") == 1


def test_save_over_the_replayed_file_is_refused(tmp_path, capsys):
    session = tmp_path / "s.log"
    session.write_bytes(HEADER + b"2026-11-08T10:15:02.120 0000:000001 0000000001 I APP: x\n")
    with pytest.raises(SystemExit) as exc:
        main(["replay", str(session), "--save", str(session)])
    assert exc.value.code == 2
    assert "is the file being replayed" in capsys.readouterr().err
    assert session.read_bytes().startswith(HEADER)  # untouched
