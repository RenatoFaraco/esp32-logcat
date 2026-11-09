"""`esp-logcat tui`: follow the device interactively (scroll, pause, change the filter live).

Threads: a worker thread runs the backend (transport -> pipeline.events -> stats, and the
--save recorder) and only puts events on a bounded queue; it never touches a widget. The UI
drains at most DRAIN_LIMIT events every 50 ms and writes them at once, so ~1 kHz of device
lines costs ~20 screen updates per second, and a fast replay of a big file is paced by the
queue (backpressure) instead of freezing the UI or growing memory. The display filter is an
immutable LogFilter swapped by the UI; the worker does not use it, so there is no lock.
Statistics are written by the worker and only read by the UI (plain ints).
"""

import queue
import time
from collections import deque
from collections.abc import Callable, Iterable
from datetime import datetime

from rich.text import Text
from textual import events as textual_events
from textual import work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.widgets import Footer, Header, Input, RichLog, Static

from esp_logcat.events import Event, Notice, NoticeKind, Received
from esp_logcat.filters import FilterSpecError, LogFilter
from esp_logcat.pipeline import events
from esp_logcat.render import device_text, render, render_notice
from esp_logcat.session import SessionWriter
from esp_logcat.stats import StreamStats

HISTORY = 10_000  # events kept for redrawing (filter change, time column, resume)
QUEUE_SIZE = 5_000  # worker -> UI; full = the worker waits (a replay is paced, not buffered)
DRAIN_LIMIT = 3_000  # events moved per 50 ms tick at most
DRAIN_INTERVAL_S = 0.05
RATE_WINDOW_S = 3.0
_END = object()  # the worker finished (input exhausted or closed)

_PORT_STATE = {
    NoticeKind.CONNECTED: "connected",
    NoticeKind.DISCONNECTED: "disconnected",
    NoticeKind.WAITING: "waiting",
    NoticeKind.SOURCE: "replaying",
}


class LogcatApp(App[None]):
    """The TUI. `chunks` is a transport's output; `close` stops the transport (from any
    thread), so the worker ends within one read timeout."""

    TITLE = "esp-logcat"
    CSS = """
    #log { height: 1fr; }
    #status { height: 2; padding: 0 1; background: $panel; }
    #filter { display: none; }
    #filter.editing { display: block; }
    """
    BINDINGS = [
        Binding("space", "toggle_pause", "pause/follow"),
        Binding("f", "edit_filter", "filter"),
        Binding("t", "toggle_time", "time"),
        Binding("c", "clear", "clear"),
        Binding("q", "quit", "quit"),
        # Coming from `stream`, Ctrl+C means "stop": make it quit here too.
        Binding("ctrl+c", "quit", "quit", show=False, priority=True),
    ]

    def __init__(
        self,
        chunks: Iterable[bytes | Notice],
        close: Callable[[], None],
        *,
        label: str,
        line_filter: LogFilter | None = None,
        recorder: SessionWriter | None = None,
        saved_file: bool = False,
        lenient: bool = False,
        clock: Callable[[], datetime] = datetime.now,
    ) -> None:
        super().__init__()
        self.stats = StreamStats()
        self.line_filter = line_filter or LogFilter()
        self.stats.filtering = not self.line_filter.is_trivial
        self.label = label
        self.state = "starting"
        self.paused = False
        self.show_time = False
        self.finished = False
        self.hidden = 0  # history lines the current filter hides
        self._chunks = chunks
        self._close = close
        self._recorder = recorder
        self._saved_file = saved_file
        self._lenient = lenient
        self._clock = clock
        self._stopping = False
        self._queue: queue.Queue[object] = queue.Queue(maxsize=QUEUE_SIZE)
        self._history: deque[Event] = deque()
        self._pending: deque[Event] = deque(maxlen=HISTORY)  # arrived while paused
        self._arrivals: deque[tuple[float, int]] = deque()
        self._last_status: tuple[object, ...] | None = None

    # ---------------------------------------------------------------- layout

    def compose(self) -> ComposeResult:
        yield Header()
        yield RichLog(id="log", max_lines=HISTORY, wrap=False, markup=False, highlight=False)
        yield Static(id="status")
        yield Input(id="filter", placeholder="logcat filter, e.g. *:W HEARTBEAT:I  (Enter / Esc)")
        yield Footer()

    def on_mount(self) -> None:
        self.sub_title = f"{self.label} · {self.state}"
        self._read()
        self.set_interval(DRAIN_INTERVAL_S, self.drain)
        self._update_status()

    # ---------------------------------------------------------------- worker thread

    @work(thread=True, exclusive=True, exit_on_error=False)
    def _read(self) -> None:
        source = events(
            self._chunks,
            self.stats,
            clock=self._clock,
            lenient=self._lenient,
            saved_file=self._saved_file,
            recorder=self._recorder,
        )
        try:
            for event in source:
                if not self._put(event):
                    return
        except Exception as exc:  # noqa: BLE001 (shown in the UI, never kills it)
            self._put(Notice(NoticeKind.DISCONNECTED, f"reader stopped: {exc}"))
        finally:
            # Close the generators here, in this thread: a transport generator left suspended
            # would keep the serial port open until the process ends.
            source.close()
            close_chunks = getattr(self._chunks, "close", None)
            if close_chunks is not None:
                close_chunks()
            self._put(_END)

    def _put(self, item: object) -> bool:
        """Waits for room in the queue, but never past a stop request (q, Ctrl+C)."""
        while not self._stopping:
            try:
                self._queue.put(item, timeout=0.1)
                return True
            except queue.Full:
                continue
        return False

    # ---------------------------------------------------------------- UI side

    def drain(self) -> None:
        """Moves queued events to the history and, unless paused, to the screen."""
        batch: list[Event] = []
        while len(batch) < DRAIN_LIMIT:
            try:
                item = self._queue.get_nowait()
            except queue.Empty:
                break
            if item is _END:
                self.finished = True
                if self.state == "replaying":
                    self.state = "ended"
                continue
            if isinstance(item, Notice) and item.kind in _PORT_STATE:
                self.state = _PORT_STATE[item.kind]  # in arrival order
            batch.append(item)  # type: ignore[arg-type]
        if batch:
            lines = 0
            for event in batch:
                if isinstance(event, Received):
                    lines += 1
                    if not self.line_filter.accepts(event.line):
                        self.stats.hidden += 1  # for the summary, like `stream`
            self._arrivals.append((time.monotonic(), lines))
            if self.paused:
                self._pending.extend(batch)
            else:
                self._remember(batch)
                self._write(batch[-HISTORY:])  # older lines would scroll out anyway
        self._update_status()

    def _remember(self, batch: Iterable[Event]) -> None:
        for event in batch:
            if len(self._history) == HISTORY:
                dropped = self._history.popleft()
                if self._is_hidden(dropped):
                    self.hidden -= 1
            self._history.append(event)
            if self._is_hidden(event):
                self.hidden += 1

    def _is_hidden(self, event: Event) -> bool:
        return isinstance(event, Received) and not self.line_filter.accepts(event.line)

    def _write(self, batch: Iterable[Event]) -> None:
        log = self.query_one("#log", RichLog)
        wrote = False
        for event in batch:
            text = self._render(event)
            if text is not None:
                log.write(text, scroll_end=False)
                wrote = True
        if wrote and not self.paused:
            log.scroll_end(animate=False)

    def _render(self, event: Event) -> Text | None:
        if isinstance(event, Notice):
            return render_notice(event)
        if not self.line_filter.accepts(event.line):
            return None
        return render(event.line, pc_time=event.pc_time if self.show_time else None)

    def _redraw(self) -> None:
        """Redraws the history (never what arrived during a pause)."""
        self.query_one("#log", RichLog).clear()
        self._write(self._history)

    def _rate(self) -> float:
        now = time.monotonic()
        while self._arrivals and now - self._arrivals[0][0] > RATE_WINDOW_S:
            self._arrivals.popleft()
        return sum(count for _, count in self._arrivals) / RATE_WINDOW_S

    def _update_status(self) -> None:
        stats = self.stats
        total = sum(stats.kinds.values())
        mode = "PAUSED" if self.paused else ("ENDED" if self.finished else "FOLLOWING")
        rate = round(self._rate())
        key = (
            total,
            stats.lost,
            stats.missed,
            stats.restarts,
            stats.kinds["InvalidLine"],
            self.hidden,
            rate,
            self.line_filter,
            mode,
            self.state,
        )
        if key == self._last_status:
            return  # nothing changed: no layout work for the terminal
        self._last_status = key
        status = Text()
        status.append(
            f"{total} lines · lost {stats.lost} · missed {stats.missed} · "
            f"restarts {stats.restarts} · invalid {stats.kinds['InvalidLine']} · "
            f"hidden {self.hidden} · {rate}/s"
        )
        status.append("\nfilter: ")
        status.append_text(device_text(self.line_filter.describe()))
        status.append(f"   {mode}", style="bold")
        self.query_one("#status", Static).update(status)
        self.sub_title = f"{self.label} · {self.state}"

    # ---------------------------------------------------------------- actions

    def action_toggle_pause(self) -> None:
        self.paused = not self.paused
        if not self.paused and self._pending:
            arrived = list(self._pending)
            self._pending.clear()
            self._remember(arrived)
            self._write(arrived[-HISTORY:])
        self._update_status()

    def action_toggle_time(self) -> None:
        self.show_time = not self.show_time
        self._redraw()

    def action_clear(self) -> None:
        self._history.clear()
        self._pending.clear()
        self.hidden = 0
        self.query_one("#log", RichLog).clear()
        self._update_status()

    def action_edit_filter(self) -> None:
        box = self.query_one("#filter", Input)
        box.value = self.line_filter.describe()
        box.add_class("editing")
        box.focus()

    def on_input_submitted(self, message: Input.Submitted) -> None:
        try:
            new_filter = LogFilter.parse(message.value)
        except FilterSpecError as exc:
            # markup=False: the message quotes what was typed ("[/]:X" must not be markup).
            self.notify(str(exc), title="filter not changed", severity="error", markup=False)
            return
        self.line_filter = new_filter
        self.stats.filtering = self.stats.filtering or not new_filter.is_trivial
        self.hidden = sum(1 for event in self._history if self._is_hidden(event))
        self._close_filter_box()
        self._redraw()
        self._update_status()

    def on_key(self, event: textual_events.Key) -> None:
        box = self.query_one("#filter", Input)
        if event.key == "escape" and box.has_class("editing"):
            self._close_filter_box()

    def _close_filter_box(self) -> None:
        self.query_one("#filter", Input).remove_class("editing")
        self.query_one("#log", RichLog).focus()

    async def action_quit(self) -> None:
        self._stop()
        self.exit()

    def on_unmount(self) -> None:
        self._stop()

    def _stop(self) -> None:
        self._stopping = True
        self._close()
