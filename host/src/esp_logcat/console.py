"""Console requests and replies (ADR-012, wire-format §2–§3): ids, pending requests, replies.

No I/O and no clock of its own: the caller passes `now` (seconds, monotonic). Thread-safe, so
the shell's reader thread can feed events while its main thread starts requests and waits.

Rules from ADR-012 §1 and §6:
- ids rotate over 1–9999, starting at a random one, and an id is not reused while a request
  with it is pending or within the reply timeout after it was taken;
- a reply line whose id has no pending request is out of context: shown as raw, never acted
  on. So is the first line after connecting, when its request was sent before that connection
  (it may be the cut end of something older); a reply to a request sent after connecting cannot
  be older than the connection, and on a quiet device it is the first line;
- a restart of the device while `reboot` or `crash` is pending is their success; for any other
  command it means the reply is lost.
"""

import random
import threading
from collections.abc import Callable
from dataclasses import dataclass, field

from esp_logcat.events import Block, Event, Notice, NoticeKind, Received, ReceivedBlock
from esp_logcat.parser import CommandEnd, CommandReply, DumpBegin, DumpEnd, parse_line

# The first word of every command the device knows (wire-format §2.1).
COMMANDS = frozenset({"info", "stats", "log", "reboot", "crash"})
RESTARTING = frozenset({"reboot", "crash"})
REPLY_TIMEOUT_S = 2.0  # ADR-012 §6
RESTART_TIMEOUT_S = 10.0  # boot to the first line takes ~1 s; a USB re-enumeration a few more
MAX_ID = 9999


class RequestIds:
    """Hands out request ids: 1–9999, rotating from a random start."""

    def __init__(
        self, start: int | None = None, rng: Callable[[int, int], int] = random.randint
    ) -> None:
        self._next = start if start is not None else rng(1, MAX_ID)

    def take(self, busy: Callable[[str], bool]) -> str:
        for _ in range(MAX_ID):
            candidate = str(self._next)
            self._next = self._next % MAX_ID + 1
            if not busy(candidate):
                return candidate
        raise RuntimeError("every request id is in use")


@dataclass
class Reply:
    """What came back for one request, complete or not."""

    id: str
    command: str  # the request without its id, e.g. "log level APP D"
    fields: list[tuple[str, str]] = field(default_factory=list)  # every data line, in order
    end: CommandEnd | None = None
    block: Block | None = None
    restarted: bool = False  # the device restarted while this request was pending
    failure: str = ""  # why there is no complete reply; empty when there is one
    elapsed: float = 0.0  # seconds from the request to the end

    @property
    def verb(self) -> str:
        return self.command.split(" ", 1)[0]

    @property
    def ok(self) -> bool:
        if self.failure:
            return False
        if self.verb == "crash":
            return self.restarted
        if self.verb == "reboot":
            return self.end is not None and self.end.ok and self.restarted
        return self.end is not None and self.end.ok


@dataclass
class _Pending:
    reply: Reply
    started: float
    deadline: float
    acknowledged: bool = False  # `reboot` said ok: now waiting for the restart


class Exchange:
    def __init__(
        self,
        ids: RequestIds | None = None,
        *,
        reply_timeout: float = REPLY_TIMEOUT_S,
        restart_timeout: float = RESTART_TIMEOUT_S,
    ) -> None:
        self._ids = ids or RequestIds()
        self._reply_timeout = reply_timeout
        self._restart_timeout = restart_timeout
        self._lock = threading.Lock()
        self._pending: dict[str, _Pending] = {}
        self._taken_at: dict[str, float] = {}  # recent ids: not reused within the timeout
        self._fresh = False  # the next line is the first after connecting
        self._connected_at = float("-inf")

    def start(self, command: str, now: float) -> str:
        """Registers a request and returns its id. Register before writing it to the port."""
        with self._lock:
            for ident, taken in list(self._taken_at.items()):
                if now - taken >= self._reply_timeout and ident not in self._pending:
                    del self._taken_at[ident]
            ident = self._ids.take(lambda candidate: candidate in self._taken_at)
            self._taken_at[ident] = now
            timeout = self._restart_timeout if command.split(" ", 1)[0] == "crash" else 0.0
            self._pending[ident] = _Pending(
                Reply(ident, command), now, now + max(timeout, self._reply_timeout)
            )
            return ident

    def cancel(self, ident: str, failure: str, now: float) -> Reply | None:
        """Ends a request that never left (the write failed)."""
        with self._lock:
            pending = self._pending.pop(ident, None)
            if pending is None:
                return None
            return self._finish(pending, now, failure=failure)

    def is_pending(self, ident: str) -> bool:
        with self._lock:
            return ident in self._pending

    expects_block = is_pending  # the Framer's question: is this #BEGIN for us?

    @property
    def pending(self) -> list[str]:
        with self._lock:
            return list(self._pending)

    def touch(self, ident: str, now: float) -> None:
        """Bytes are arriving for this request's block: its timeout restarts (ADR-012 §6: at most
        2 s without bytes inside a block)."""
        with self._lock:
            pending = self._pending.get(ident)
            if pending is not None and not pending.acknowledged:
                pending.deadline = max(pending.deadline, now + self._reply_timeout)

    def observe(self, event: Event, now: float) -> tuple[list[Reply], bool]:
        """Feeds one pipeline event. Returns the requests it completed, and whether the event is
        a reply line out of context (to be shown as raw)."""
        with self._lock:
            match event:
                case Notice(kind=NoticeKind.CONNECTED):
                    self._fresh = True
                    self._connected_at = now
                    return [], False
                case Notice(kind=NoticeKind.DISCONNECTED):
                    return self._restart_or_lose("the port closed", now), False
                case Notice(kind=NoticeKind.RESTART):
                    return self._restart_or_lose("the device restarted", now), False
                case ReceivedBlock(block=block):
                    self._fresh = False
                    return self._block(block, now)
                case Received(line=line):
                    fresh, self._fresh = self._fresh, False
                    if isinstance(line, CommandReply | CommandEnd | DumpBegin | DumpEnd):
                        return self._reply_line(line, now, fresh)
                    return [], False
                case _:
                    return [], False

    def expire(self, now: float) -> list[Reply]:
        """Ends the requests whose time is up."""
        with self._lock:
            done = []
            for ident, pending in list(self._pending.items()):
                if now < pending.deadline:
                    continue
                del self._pending[ident]
                if pending.acknowledged or pending.reply.verb == "crash":
                    failure = f"no restart seen in {self._restart_timeout:g} s"
                else:
                    failure = f"no reply in {self._reply_timeout:g} s"
                done.append(self._finish(pending, now, failure=failure))
            return done

    def _reply_line(
        self, line: CommandReply | CommandEnd | DumpBegin | DumpEnd, now: float, fresh: bool
    ) -> tuple[list[Reply], bool]:
        pending = self._pending.get(line.id)
        if pending is not None and fresh and pending.started < self._connected_at:
            pending = None
        # A #BEGIN or #END that reaches us as a line was not framed as a block: out of context.
        if pending is None or isinstance(line, DumpBegin | DumpEnd):
            return [], True
        if isinstance(line, CommandReply):
            pending.reply.fields.extend(line.fields)
            return [], False
        pending.reply.end = line
        if pending.reply.verb == "reboot" and line.ok:
            pending.acknowledged = True  # done when the restart shows up
            pending.deadline = now + self._restart_timeout
            return [], False
        del self._pending[line.id]
        return [self._finish(pending, now)], False

    def _block(self, block: Block, now: float) -> tuple[list[Reply], bool]:
        pending = self._pending.get(block.id)
        if pending is None:
            return [], True
        pending.reply.block = block
        if block.ok:
            pending.deadline = max(pending.deadline, now + self._reply_timeout)
            return [], False
        # Its terminator may have gone into the block's byte count: do not wait for it.
        del self._pending[block.id]
        return [self._finish(pending, now, failure=f"dump block damaged: {block.problem}")], False

    def _restart_or_lose(self, why: str, now: float) -> list[Reply]:
        done = []
        for ident, pending in list(self._pending.items()):
            del self._pending[ident]
            if pending.reply.verb in RESTARTING:
                pending.reply.restarted = True
                done.append(self._finish(pending, now))
            else:
                done.append(self._finish(pending, now, failure=f"no reply: {why}"))
        return done

    def _finish(self, pending: _Pending, now: float, *, failure: str = "") -> Reply:
        reply = pending.reply
        reply.failure = failure
        reply.elapsed = now - pending.started
        return reply


def fields_value(value: str) -> int | str:
    """A value as JSON wants it: digits become a number (wire values are never signed)."""
    return int(value) if value.isdigit() else value


def reply_json(reply: Reply) -> dict[str, object]:
    """The reply as a JSON object (ADR-012 §6: JSON is made on the host, never on the wire).

    A key that comes more than once (one line per sink, say) becomes a list of its values."""
    fields: dict[str, object] = {}
    for key, raw in reply.fields:
        value = fields_value(raw)
        if key not in fields:
            fields[key] = value
        elif isinstance(fields[key], list):
            fields[key].append(value)  # type: ignore[union-attr]
        else:
            fields[key] = [fields[key], value]
    out: dict[str, object] = {
        "id": fields_value(reply.id),
        "command": reply.command,
        "ok": reply.ok,
    }
    if fields:
        out["fields"] = fields
    if reply.block is not None:
        out["block"] = {
            "name": reply.block.name,
            "bytes": reply.block.size,
            "crc_ok": reply.block.ok,
            "lines": block_lines(reply.block),
        }
    if reply.end is not None and not reply.end.ok:
        out["code"] = reply.end.code
        if reply.end.text:
            out["text"] = reply.end.text
    if reply.verb in RESTARTING:
        out["restarted"] = reply.restarted
    if reply.failure:
        out["error"] = reply.failure
    out["elapsed_ms"] = round(reply.elapsed * 1000)
    return out


def block_lines(block: Block) -> list[str]:
    """The block's lines as text, for JSON: bytes outside ASCII as \\xNN escapes."""
    return [line.decode("ascii", "backslashreplace") for line in block.content.split(b"\n") if line]


def block_parsed(block: Block) -> list[object]:
    """The block's lines through the log parser (wire-format §3): garbage and torn lines are
    classified, never shown as records."""
    pieces = block.content.split(b"\n")
    lines = [piece + b"\n" for piece in pieces[:-1]]
    if pieces[-1]:
        lines.append(pieces[-1])  # no final newline: a torn line
    return [parse_line(line, assume_log=True) for line in lines]
