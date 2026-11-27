"""`esp-logcat shell`: console commands to the device, replies back (PRD H4, ADR-012).

A frontend over the same backend as `stream` (ADR-008): a reader thread runs the pipeline with
a Framer (dump blocks by byte count) and feeds the Exchange; the main thread sends requests and
waits for their replies. No textual here; prompt_toolkit only for the interactive prompt.
"""

import json
import threading
import time
from collections.abc import Callable, Iterable, Iterator
from typing import Protocol, TextIO

from rich.console import Console
from rich.text import Text

from esp_logcat.console import COMMANDS, Exchange, Reply, block_parsed, reply_json
from esp_logcat.events import Notice, NoticeKind, Received, ReceivedBlock
from esp_logcat.filters import LogFilter
from esp_logcat.framing import Framer
from esp_logcat.parser import CommandEnd, CommandReply, DumpBegin, DumpEnd, RawLine
from esp_logcat.pipeline import events, visible
from esp_logcat.render import device_text, render, render_notice
from esp_logcat.stats import StreamStats

CONNECT_TIMEOUT_S = 3.0
# A block's bytes extend its request only up to size / this rate + 2 s (the native port does
# hundreds of kB/s; this is a floor, not an estimate).
BLOCK_MIN_RATE = 20_000
HELP = """\
commands (the shell adds the request id):
  info                      who the device is: firmware, chip, uptime, reset reason
  stats                     logging counters: dropped lines, queue, stack, sinks
  log level <tag> <L>       set a tag's level at run time: E W I D V, or N to silence; * = default
  log tail                  the last lines kept in RAM
  log sync                  make what was logged durable in flash now (ok = it is there)
  log dump | log clear      the logs in flash (when the firmware has them)
  reboot                    restart the device
  crash panic | crash wdt   crash the device on purpose (recovery tests)
shell: help, quit (or Ctrl+D)"""


class Transport(Protocol):
    """What the shell needs from SerialTransport (tests provide a fake)."""

    def chunks(self) -> Iterator[bytes | Notice]: ...

    def write(self, data: bytes) -> bool: ...

    def close(self) -> None: ...


class _Closed(Exception):
    """The shell closed its own port: the reader stops without flushing a half line."""


class Shell:
    """Owns the reader thread. `request()` sends one command and waits for its reply."""

    def __init__(
        self,
        transport: Transport,
        out: Console,
        err: Console,
        *,
        show_logs: bool = False,
        line_filter: LogFilter | None = None,
        exchange: Exchange | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._transport = transport
        self._out = out
        self._err = err
        self._show_logs = show_logs
        self._filter = line_filter
        self._clock = clock
        self.exchange = exchange or Exchange()
        self.stats = StreamStats()
        self._framer = Framer(self.exchange.expects_block)
        self._done: dict[str, Reply] = {}
        self._changed = threading.Condition()
        self._connected = threading.Event()
        self._reader_done = threading.Event()
        self.waiting = ""  # the last "waiting for the port" notice, to explain a failure
        self._reader_error = ""
        # Set by close(): what the reader gets after it was cut by us (#82), never shown.
        self._closing = threading.Event()
        self._thread = threading.Thread(target=self._read, name="esp-logcat-reader", daemon=True)

    # ------------------------------------------------------------------ lifecycle

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        # First: closing the port ends the stream, and the line that was arriving comes out as a
        # torn piece. The host cut it, not the device: it is neither shown nor counted.
        self._closing.set()
        self._transport.close()
        self._thread.join(timeout=2.0)

    def wait_connected(self, timeout: float) -> bool:
        return self._connected.wait(timeout)

    @property
    def reader_error(self) -> str:
        """Why the reader thread stopped; empty while it runs."""
        if not self._reader_done.is_set():
            return ""
        return self._reader_error or "the input ended"

    # ------------------------------------------------------------------ requests

    def request(self, command: str) -> Reply:
        """Sends one command and waits for its reply. Ctrl+C abandons the request (and the block
        it may be reading) and is raised again for the caller."""
        command = " ".join(command.split())  # one line: no newline can start a second request
        if not self._connected.is_set():
            # After a reboot the native port re-enumerates: wait for it like at the start.
            self.wait_connected(CONNECT_TIMEOUT_S)
        ident = self.exchange.start(command, self._clock())
        if not self._transport.write(f"{ident} {command}\n".encode("ascii", "replace")):
            reply = self.exchange.cancel(ident, "the port is not open", self._clock())
            if reply is not None:
                return reply
            # Otherwise the reader ended it first (a restart or a disconnect): it is in _done.
        try:
            return self._wait(ident)
        except KeyboardInterrupt:
            self._abandon(ident, "interrupted")
            raise

    def _wait(self, ident: str) -> Reply:
        with self._changed:
            while ident not in self._done:
                for reply in self.exchange.expire(self._clock()):
                    self._drop_block(reply.id)
                    self._done[reply.id] = reply
                if ident not in self._done and self._reader_done.is_set():
                    self._abandon(ident, f"the reader stopped: {self.reader_error}")
                if ident not in self._done:
                    self._changed.wait(0.05)
            return self._done.pop(ident)

    def _abandon(self, ident: str, why: str) -> None:
        reply = self.exchange.cancel(ident, why, self._clock())
        self._drop_block(ident)
        if reply is not None:
            self._done[ident] = reply

    def _drop_block(self, ident: str) -> None:
        if self._framer.block_id == ident:
            self._framer.cancel_block()

    # ------------------------------------------------------------------ reader thread

    def _chunks(self, source: Iterator[bytes | Notice]) -> Iterator[bytes | Notice]:
        # (request id, block number, start): a `log dump` sends several blocks under one id, and
        # each gets its own size / rate allowance from its own start.
        block_started: tuple[str, int, float] | None = None
        for item in source:
            block = self._framer.block_id
            if block is None:
                block_started = None
            elif isinstance(item, bytes):
                now = self._clock()
                number = self._framer.blocks_begun
                if block_started is None or block_started[:2] != (block, number):
                    block_started = (block, number, now)
                # Bytes keep a block's request alive, but not forever: a block aborted inside
                # (wire-format §3) is followed by live log, which would otherwise be read as
                # content until <bytes> is reached.
                limit = block_started[2] + self._framer.block_size / BLOCK_MIN_RATE + 2.0
                self.exchange.touch(block, now, limit)
            yield item

    def _closing_ends(self, chunks: Iterator[bytes | Notice]) -> Iterator[bytes | Notice]:
        """Stops the input as soon as close() was called. Raising (not returning) keeps the
        pipeline from flushing the half line it holds, which it does when its input ends."""
        for item in chunks:
            if self._closing.is_set():
                raise _Closed
            yield item
        if self._closing.is_set():
            raise _Closed

    def _read(self) -> None:
        source = self._transport.chunks()
        try:
            parsed = events(
                self._closing_ends(self._chunks(source)), self.stats, framer=self._framer
            )
            for event in visible(parsed, self.stats, self._filter if self._show_logs else None):
                replies, out_of_context = self.exchange.observe(event, self._clock())
                if isinstance(event, Notice):
                    if event.kind is NoticeKind.CONNECTED:
                        self._connected.set()
                    elif event.kind is NoticeKind.DISCONNECTED:
                        self._connected.clear()
                    elif event.kind is NoticeKind.WAITING:
                        self.waiting = event.text
                if replies:
                    with self._changed:
                        for reply in replies:
                            self._done[reply.id] = reply
                        self._changed.notify_all()
                self._show(event, out_of_context)
        except _Closed:
            pass  # close() was called: nothing after it is shown, nothing is an error
        except Exception as exc:  # noqa: BLE001  (reported to the waiting request, not lost)
            self._reader_error = f"{type(exc).__name__}: {exc}"
        finally:
            # The transport closes its port in its own `finally`: run it now, not whenever the
            # stopped generator is collected.
            close = getattr(source, "close", None)
            if close is not None:
                close()
            self._reader_done.set()
            with self._changed:
                self._changed.notify_all()

    def _show(self, event: object, out_of_context: bool) -> None:
        if self._show_logs:
            show_event(event, out_of_context, self._out, self._err)


def show_event(event: object, out_of_context: bool, out: Console, err: Console) -> None:
    """How the interactive shell shows one pipeline event (also used for the manual's pictures).

    Log lines print as in `stream` (the filter was applied by visible()); reply lines belong to
    their reply and print with it; a reply line nobody asked for prints as raw (ADR-012 §6)."""
    match event:
        case Received(line=line) if out_of_context:
            out.print(render(RawLine(render(line).plain)))
        case Received(line=line) if _is_reply_line(line):
            pass
        case Received(line=line):
            out.print(render(line))
        case Notice():
            err.print(render_notice(event))
        case ReceivedBlock():
            pass


def _is_reply_line(line: object) -> bool:
    return isinstance(line, CommandReply | CommandEnd | DumpBegin | DumpEnd)


# ------------------------------------------------------------------ printing a reply


def print_reply(reply: Reply, out: Console, err: Console, *, as_json: bool = False) -> int:
    """Prints a reply; returns the exit code: 0 when it worked, 1 otherwise."""
    if as_json:
        out.print(Text(json.dumps(reply_json(reply))), soft_wrap=True)
        return 0 if reply.ok else 1
    width = max((len(key) for key, _ in reply.fields), default=0)
    for key, value in reply.fields:
        text = Text(key.ljust(width), style="bold")
        text.append("  ")
        text.append(value)
        out.print(text)
    for block in reply.blocks:
        # A file's block (`log dump`) says which file it closes; the RAM tail needs no name.
        name = f"{block.name}: " if block.name != "ram" else ""
        if not block.ok:
            # Not the file's content (wire-format §3: discarded): only say what went wrong.
            out.print(Text(f"{name}{block.size} bytes, {block.problem}", style="dim"), markup=False)
            continue
        parsed = block_parsed(block)
        for line in parsed:
            out.print(render(line))
        out.print(
            Text(f"{name}{len(parsed)} lines, {block.size} bytes, CRC ok", style="dim"),
            markup=False,
        )
    if reply.failure:
        err.print(Text(f"error: {reply.failure}", style="bold red"))
    elif reply.end is not None and not reply.end.ok:
        detail = f": {reply.end.text}" if reply.end.text else ""
        err.print(Text(f"error: {reply.end.code}{detail}", style="bold red"))
    elif reply.restarted:
        out.print(Text("ok: the device restarted", style="green"))
    elif not reply.fields and not reply.blocks:
        out.print(Text("ok", style="green"))
    return 0 if reply.ok else 1


# ------------------------------------------------------------------ the two modes


def run_one(shell: Shell, words: list[str], out: Console, err: Console, *, as_json: bool) -> int:
    if not shell.wait_connected(CONNECT_TIMEOUT_S):
        err.print(device_text(f"esp-logcat: {shell.waiting or 'the port did not open'}"))
        return 1
    return print_reply(shell.request(" ".join(words)), out, err, as_json=as_json)


def run_lines(shell: Shell, lines: Iterable[str], out: Console, err: Console) -> int:
    """Commands from a pipe, one per line, no prompt. Exit code 1 if any of them failed."""
    if not shell.wait_connected(CONNECT_TIMEOUT_S):
        err.print(device_text(f"esp-logcat: {shell.waiting or 'the port did not open'}"))
        return 1
    code = 0
    for number, raw in enumerate(lines):
        if number == 0:
            raw = raw.removeprefix("\ufeff")  # Windows PowerShell puts a BOM before piped text
        command = " ".join(raw.split())
        if not command or command.startswith("#"):
            continue
        result = handle(shell, command, out, err)
        if result is None:
            break
        code = max(code, result)
        if shell.reader_error:
            err.print(device_text(f"esp-logcat: {shell.reader_error}"))
            return 1
    return code


def handle(shell: Shell, command: str, out: Console, err: Console) -> int | None:
    """One typed line: a built-in or a device command. None means quit."""
    if command in ("quit", "exit"):
        return None
    if command == "help":
        out.print(HELP, markup=False, highlight=False)
        return 0
    if command.split(" ", 1)[0] not in COMMANDS:
        err.print(f"unknown command: {command} (help lists them)", markup=False)
        return 1
    return print_reply(shell.request(command), out, err)


def run_prompt(shell: Shell, out: Console, err: Console) -> int:
    """The interactive prompt: log lines print above the line being typed."""
    from prompt_toolkit import PromptSession
    from prompt_toolkit.history import InMemoryHistory
    from prompt_toolkit.patch_stdout import patch_stdout

    session: PromptSession[str] = PromptSession(history=InMemoryHistory())
    out.print("esp-logcat shell: type help, quit or Ctrl+D to leave", style="dim")
    code = 0
    with patch_stdout(raw=True):
        while True:
            try:
                command = " ".join(session.prompt("logcat> ").split())
            except KeyboardInterrupt:
                continue  # Ctrl+C clears the line, like a terminal
            except EOFError:
                break
            if not command:
                continue
            try:
                result = handle(shell, command, out, err)
            except KeyboardInterrupt:
                err.print("interrupted", style="dim")
                continue
            if result is None:
                break
            code = max(code, result)
            if shell.reader_error:
                err.print(device_text(f"esp-logcat: {shell.reader_error}"))
                return 1
    return code


def is_command(words: list[str]) -> bool:
    return bool(words) and words[0] in COMMANDS


def stdin_is_terminal(stream: TextIO) -> bool:
    isatty = getattr(stream, "isatty", None)
    return bool(isatty and isatty())
