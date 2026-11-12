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
from esp_logcat.pipeline import events
from esp_logcat.render import render, render_notice
from esp_logcat.stats import StreamStats

CONNECT_TIMEOUT_S = 3.0
HELP = """\
commands (the shell adds the request id):
  info                      who the device is: firmware, chip, uptime, reset reason
  stats                     logging counters: dropped lines, queue, stack, sinks
  log level <tag> <L>       set a tag's level at run time: E W I D V, or N to silence; * = default
  log tail                  the last lines kept in RAM
  log dump | log clear      the logs in flash (when the firmware has them)
  reboot                    restart the device
  crash panic | crash wdt   crash the device on purpose (recovery tests)
shell: help, quit (or Ctrl+D)"""


class Transport(Protocol):
    """What the shell needs from SerialTransport (tests provide a fake)."""

    def chunks(self) -> Iterator[bytes | Notice]: ...

    def write(self, data: bytes) -> bool: ...

    def close(self) -> None: ...


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
        self._thread = threading.Thread(target=self._read, name="esp-logcat-reader", daemon=True)

    # ------------------------------------------------------------------ lifecycle

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        self._transport.close()
        self._thread.join(timeout=2.0)

    def wait_connected(self, timeout: float) -> bool:
        return self._connected.wait(timeout)

    # ------------------------------------------------------------------ requests

    def request(self, command: str) -> Reply:
        ident = self.exchange.start(command, self._clock())
        if not self._transport.write(f"{ident} {command}\n".encode("ascii", "replace")):
            reply = self.exchange.cancel(ident, "the port is not open", self._clock())
            assert reply is not None
            return reply
        with self._changed:
            while ident not in self._done:
                for reply in self.exchange.expire(self._clock()):
                    if self._framer.block_id == reply.id:
                        self._framer.cancel_block()
                    self._done[reply.id] = reply
                if ident not in self._done:
                    self._changed.wait(0.05)
            return self._done.pop(ident)

    # ------------------------------------------------------------------ reader thread

    def _chunks(self) -> Iterator[bytes | Notice]:
        for item in self._transport.chunks():
            if isinstance(item, bytes) and (block := self._framer.block_id) is not None:
                self.exchange.touch(block, self._clock())
            yield item

    def _read(self) -> None:
        try:
            for event in events(self._chunks(), self.stats, framer=self._framer):
                replies, out_of_context = self.exchange.observe(event, self._clock())
                self._show(event, out_of_context)
                if isinstance(event, Notice):
                    if event.kind is NoticeKind.CONNECTED:
                        self._connected.set()
                    elif event.kind is NoticeKind.WAITING:
                        self.waiting = event.text
                if replies:
                    with self._changed:
                        for reply in replies:
                            self._done[reply.id] = reply
                        self._changed.notify_all()
        finally:
            self._reader_done.set()
            with self._changed:
                self._changed.notify_all()

    def _show(self, event: object, out_of_context: bool) -> None:
        if not self._show_logs:
            return
        match event:
            case Received(line=line) if out_of_context:
                # A reply nobody asked for: shown, never acted on (ADR-012 §6).
                self._out.print(render(RawLine(render(line).plain)))
            case Received(line=line) if _is_reply_line(line):
                pass  # part of a reply: printed with the reply
            case Received(line=line):
                if self._filter is None or self._filter.accepts(line):
                    self._out.print(render(line))
            case Notice(kind=kind) if kind is not NoticeKind.FILTER:
                self._err.print(render_notice(event))
            case ReceivedBlock() | Notice():
                pass


def _is_reply_line(line: object) -> bool:
    return isinstance(line, CommandReply | CommandEnd | DumpBegin | DumpEnd)


# ------------------------------------------------------------------ printing a reply


def print_reply(reply: Reply, out: Console, err: Console, *, as_json: bool = False) -> int:
    """Prints a reply; returns the exit code: 0 when it worked, 1 otherwise."""
    if as_json:
        out.file.write(json.dumps(reply_json(reply)) + "\n")
        out.file.flush()
        return 0 if reply.ok else 1
    width = max((len(key) for key, _ in reply.fields), default=0)
    for key, value in reply.fields:
        text = Text(key.ljust(width), style="bold")
        text.append("  ")
        text.append(value)
        out.print(text)
    if reply.block is not None:
        parsed = block_parsed(reply.block)
        for line in parsed:
            out.print(render(line))
        lines = len(parsed)
        status = "CRC ok" if reply.block.ok else reply.block.problem
        out.print(
            Text(f"{lines} lines, {reply.block.size} bytes, {status}", style="dim"), markup=False
        )
    if reply.failure:
        err.print(Text(f"error: {reply.failure}", style="bold red"))
    elif reply.end is not None and not reply.end.ok:
        detail = f": {reply.end.text}" if reply.end.text else ""
        err.print(Text(f"error: {reply.end.code}{detail}", style="bold red"))
    elif reply.restarted:
        out.print(Text("ok: the device restarted", style="green"))
    elif not reply.fields and reply.block is None:
        out.print(Text("ok", style="green"))
    return 0 if reply.ok else 1


# ------------------------------------------------------------------ the two modes


def run_one(shell: Shell, words: list[str], out: Console, err: Console, *, as_json: bool) -> int:
    if not shell.wait_connected(CONNECT_TIMEOUT_S):
        err.print(f"esp-logcat: {shell.waiting or 'the port did not open'}", markup=False)
        return 1
    return print_reply(shell.request(" ".join(words)), out, err, as_json=as_json)


def run_lines(shell: Shell, lines: Iterable[str], out: Console, err: Console) -> int:
    """Commands from a pipe, one per line, no prompt. Exit code 1 if any of them failed."""
    if not shell.wait_connected(CONNECT_TIMEOUT_S):
        err.print(f"esp-logcat: {shell.waiting or 'the port did not open'}", markup=False)
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
            result = handle(shell, command, out, err)
            if result is None:
                break
            code = max(code, result)
    return code


def is_command(words: list[str]) -> bool:
    return bool(words) and words[0] in COMMANDS


def stdin_is_terminal(stream: TextIO) -> bool:
    isatty = getattr(stream, "isatty", None)
    return bool(isatty and isatty())
