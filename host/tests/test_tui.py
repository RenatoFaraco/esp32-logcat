"""The TUI under Textual's Pilot: no terminal, no board (Done when of #56)."""

import asyncio
import io
import queue
from collections.abc import Iterator
from pathlib import Path

from esp_logcat.events import Notice, NoticeKind
from esp_logcat.filters import LogFilter
from esp_logcat.session import HEADER, SessionWriter
from esp_logcat.transport import FileTransport
from esp_logcat.tui.app import LogcatApp

TWO_BOOTS = Path(__file__).parent / "fixtures" / "two_boots_esp32c3.log"


def screen_lines(app: LogcatApp) -> list[str]:
    log = app.query_one("#log")
    return ["".join(segment.text for segment in strip).rstrip() for strip in log.lines]


def status(app: LogcatApp) -> str:
    return str(app.query_one("#status").render())


def replay_app(**kwargs: object) -> LogcatApp:
    transport = FileTransport.open(str(TWO_BOOTS))
    return LogcatApp(transport.chunks(), transport.close, label="replay", saved_file=True, **kwargs)


async def settle(pilot: object, app: LogcatApp) -> None:
    for _ in range(100):  # until the worker is done and the queue drained
        await pilot.pause(0.02)
        if app.finished:
            await pilot.pause(0.1)
            return
    raise AssertionError("the worker did not finish")


class Feed:
    """A source the test controls: chunks are pushed while the app runs."""

    def __init__(self) -> None:
        self.items: queue.SimpleQueue[object] = queue.SimpleQueue()
        self.closed = False

    def chunks(self) -> Iterator[bytes | Notice]:
        while not self.closed:
            try:
                item = self.items.get(timeout=0.02)
            except queue.Empty:
                continue
            yield item

    def close(self) -> None:
        self.closed = True


def line(seq: int, level: str = "I", tag: str = "APP", msg: str = "m") -> bytes:
    return f"0000:{seq:06d} {seq * 10:010d} {level} {tag}: {msg}\n".encode()


def run(coro_factory) -> None:
    asyncio.run(coro_factory())


# ---------------------------------------------------------------- replay of the boot fixture


def test_replay_shows_the_lines_and_the_counters():
    async def scenario() -> None:
        app = replay_app()
        async with app.run_test(size=(150, 40)) as pilot:
            await settle(pilot, app)
            lines = screen_lines(app)
            assert any("W LOGCAT: dropped 72 lines: log queue full" in text for text in lines)
            assert any("device restarted" in text for text in lines)
            text = status(app)
            assert "189 lines" in text and "lost 144" in text and "restarts 1" in text
            assert app.state == "ended"

    run(scenario)


def test_filter_edited_live_redraws_the_history():
    async def scenario() -> None:
        app = replay_app()
        async with app.run_test(size=(150, 40)) as pilot:
            await settle(pilot, app)
            await pilot.press("f")
            box = app.query_one("#filter")
            box.value = ""
            await pilot.press(*"*:W", "enter")
            await pilot.pause(0.1)
            logcat_lines = [t for t in screen_lines(app) if t[:4].isdigit()]
            assert logcat_lines and {t.split()[2] for t in logcat_lines} == {"W", "E"}
            assert "filter: *:W" in status(app)
            assert "hidden 163" in status(app)

    run(scenario)


def test_invalid_filter_keeps_the_previous_one():
    async def scenario() -> None:
        app = replay_app(line_filter=LogFilter.parse("*:W"))
        async with app.run_test(size=(150, 40)) as pilot:
            await settle(pilot, app)
            await pilot.press("f")
            app.query_one("#filter").value = ""
            await pilot.press(*"APP:F", "enter")
            await pilot.pause(0.1)
            assert app.line_filter == LogFilter.parse("*:W")
            assert app.query_one("#filter").has_class("editing")  # still open to fix it
            await pilot.press("escape")
            assert not app.query_one("#filter").has_class("editing")

    run(scenario)


def test_time_column_and_clear():
    async def scenario() -> None:
        session = HEADER + b"2026-11-08T10:15:02.120 0000:000001 0000000010 I APP: timed\n"
        transport = FileTransport(io.BytesIO(session), "s.log")
        app = LogcatApp(transport.chunks(), transport.close, label="r", saved_file=True)
        async with app.run_test(size=(120, 20)) as pilot:
            await settle(pilot, app)
            await pilot.press("t")
            assert "10:15:02.120 0000:000001 0000000010 I APP: timed" in screen_lines(app)
            await pilot.press("c")
            assert screen_lines(app) == []
            assert "1 lines" in status(app)  # counters survive a clear

    run(scenario)


# ---------------------------------------------------------------- live source


def test_pause_holds_the_view_and_resume_shows_what_arrived():
    async def scenario() -> None:
        feed = Feed()
        app = LogcatApp(feed.chunks(), feed.close, label="COM6")
        async with app.run_test(size=(120, 30)) as pilot:
            feed.items.put(Notice(NoticeKind.CONNECTED, "connected to COM6"))
            feed.items.put(line(1))
            await pilot.pause(0.2)
            assert app.state == "connected" and "COM6 · connected" in app.sub_title
            await pilot.press("space")
            feed.items.put(line(2, msg="while paused"))
            await pilot.pause(0.2)
            assert not any("while paused" in t for t in screen_lines(app))
            assert "2 lines" in status(app) and "PAUSED" in status(app)
            await pilot.press("space")
            await pilot.pause(0.1)
            assert any("while paused" in t for t in screen_lines(app))
            feed.items.put(Notice(NoticeKind.DISCONNECTED, "COM6: connection lost"))
            await pilot.pause(0.2)
            assert app.state == "disconnected"
            await pilot.press("q")
        assert feed.closed

    run(scenario)


def test_a_burst_is_drawn_in_batches():
    async def scenario() -> None:
        feed = Feed()
        app = LogcatApp(feed.chunks(), feed.close, label="COM6")
        writes = []
        async with app.run_test(size=(120, 30)) as pilot:
            log = app.query_one("#log")
            original = log.write
            log.write = lambda text, *a, **k: writes.append(1) or original(text, *a, **k)
            refreshes = 0
            original_status = app._update_status

            def counting() -> None:
                nonlocal refreshes
                refreshes += 1
                original_status()

            app._update_status = counting
            feed.items.put(b"".join(line(seq) for seq in range(1, 5001)))
            for _ in range(100):  # drained in capped batches (DRAIN_LIMIT per tick)
                await pilot.pause(0.05)
                if len(writes) == 5000:
                    break
            assert len(writes) == 5000
            assert "5000 lines" in status(app)
            assert refreshes < 40  # a few drains of 50 ms, not one per line
            await pilot.press("q")

    run(scenario)


def test_save_records_the_session_while_showing(tmp_path):
    async def scenario() -> None:
        feed = Feed()
        saved = tmp_path / "s.log"
        recorder = SessionWriter(saved.open("wb"))
        app = LogcatApp(
            feed.chunks(),
            feed.close,
            label="COM6",
            recorder=recorder,
            line_filter=LogFilter.parse("*:S"),
        )
        async with app.run_test(size=(120, 30)) as pilot:
            feed.items.put(line(1, msg="hidden but saved"))
            await pilot.pause(0.2)
            await pilot.press("q")
        recorder.close()
        assert b"hidden but saved" in saved.read_bytes()

    run(scenario)


# ---------------------------------------------------------------- review findings (#56)


def test_device_text_and_typed_filters_are_never_markup():
    async def scenario() -> None:
        feed = Feed()
        app = LogcatApp(feed.chunks(), feed.close, label="COM6")
        async with app.run_test(size=(140, 30)) as pilot:
            feed.items.put(
                b"0000:000001 0000000010 I APP: [bold]x[/] \x1b[2J " + "\u202e".encode() + b"\n"
            )
            await pilot.pause(0.2)
            shown = "\n".join(screen_lines(app))
            assert "[bold]x[/]" in shown and "\\x1b[2J" in shown and "\\xe2\\x80\\xae" in shown
            assert chr(27) not in shown  # no control byte reaches the terminal
            await pilot.press("f")
            app.query_one("#filter").value = ""
            await pilot.press(*"[/]:X", "enter")  # used to crash the app (markup in notify)
            await pilot.pause(0.2)
            assert app.is_running
            await pilot.press("escape", "q")

    run(scenario)


def test_typing_a_filter_does_not_trigger_the_shortcuts():
    async def scenario() -> None:
        app = replay_app()
        async with app.run_test(size=(150, 40)) as pilot:
            await settle(pilot, app)
            await pilot.press("f")
            app.query_one("#filter").value = ""
            await pilot.press(*"*:S HEARTBEAT:I fcqt", "enter")  # space, f, c, q, t typed
            await pilot.pause(0.1)
            assert app.is_running and not app.paused and not app.show_time
            assert app.line_filter.describe() == "*:S HEARTBEAT:I fcqt:V"

    run(scenario)


def test_worker_failure_is_shown_and_the_source_is_closed():
    closed = []

    def failing():
        try:
            yield Notice(NoticeKind.CONNECTED, "connected to COM6")
            yield line(1)
            raise RuntimeError("boom")
        finally:
            closed.append(True)

    async def scenario() -> None:
        app = LogcatApp(failing(), lambda: None, label="COM6")
        async with app.run_test(size=(120, 30)) as pilot:
            await settle(pilot, app)
            assert any("reader stopped: boom" in t for t in screen_lines(app))
            assert app.is_running
            await pilot.press("q")
        assert closed == [True]

    run(scenario)


def test_time_or_filter_change_while_paused_keeps_the_pause():
    async def scenario() -> None:
        feed = Feed()
        app = LogcatApp(feed.chunks(), feed.close, label="COM6")
        async with app.run_test(size=(120, 30)) as pilot:
            feed.items.put(line(1, msg="before"))
            await pilot.pause(0.2)
            await pilot.press("space")
            feed.items.put(line(2, msg="during the pause"))
            await pilot.pause(0.2)
            await pilot.press("t")
            assert not any("during the pause" in t for t in screen_lines(app))
            await pilot.press("space")
            await pilot.pause(0.1)
            assert any("during the pause" in t for t in screen_lines(app))
            await pilot.press("q")

    run(scenario)


def test_hidden_counts_the_history_and_clear_resets_it():
    async def scenario() -> None:
        feed = Feed()
        app = LogcatApp(feed.chunks(), feed.close, label="COM6", line_filter=LogFilter.parse("*:W"))
        async with app.run_test(size=(120, 30)) as pilot:
            feed.items.put(b"".join(line(seq) for seq in range(1, 12_001)))  # all I: hidden
            # The status reads the worker's stats, which are ahead of the UI: wait until the
            # UI has drained the queue too, not just until the count shows up.
            for _ in range(400):
                await pilot.pause(0.05)
                if sum(app.stats.kinds.values()) == 12_000 and app._queue.empty():
                    await pilot.pause(0.1)
                    break
            assert app.hidden == 10_000  # only what the history still holds
            await pilot.press("f")
            app.query_one("#filter").value = ""
            await pilot.press(*"*:V", "enter")
            await pilot.pause(0.1)
            assert app.hidden == 0
            await pilot.press("c")
            assert app.hidden == 0 and "hidden 0" in status(app)
            await pilot.press("q")
        assert app.stats.hidden == 12_000  # the summary counts every hidden line, like stream

    run(scenario)


def test_quit_stays_responsive_while_a_big_replay_is_paced(tmp_path):
    async def scenario() -> None:
        big = tmp_path / "big.log"
        big.write_bytes(b"".join(line(seq % 999_999) for seq in range(200_000)))
        transport = FileTransport.open(str(big))
        app = LogcatApp(transport.chunks(), transport.close, label="r", saved_file=True)
        async with app.run_test(size=(120, 30)) as pilot:
            await pilot.pause(0.3)
            assert app._queue.qsize() <= 5_000  # bounded: the worker waits, memory does not grow
            await pilot.press("q")

    run(scenario)


def test_cli_tui_rejects_stdin_and_missing_files(capsys, tmp_path):
    from esp_logcat.cli import main

    for argv, message in (
        (["tui", "--replay", "-"], "tui cannot replay stdin"),
        (["tui", "--replay", str(tmp_path / "nope.log")], "cannot read"),
    ):
        try:
            main(argv)
        except SystemExit as exc:
            assert exc.code == 2
        assert message in capsys.readouterr().err


def test_cli_tui_prints_the_summary_after_closing(capsys, monkeypatch):
    from esp_logcat import cli
    from esp_logcat.tui import app as tui_app

    def fake_run(self) -> None:  # the app exits at once; the summary must still be printed
        self.stats.kinds["LogLine"] += 3

    monkeypatch.setattr(tui_app.LogcatApp, "run", fake_run)
    assert cli.main(["tui", "--replay", str(TWO_BOOTS)]) == 0
    assert "3 lines, 3 log" in capsys.readouterr().err
