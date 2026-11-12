"""Device bytes to lines and dump blocks: a block is read by byte count, never split at newlines.

A dump block (wire-format §3) carries file content as it is: DEL sentinels, erased flash
(0xFF), NULs, runs without a newline. The line splitter would cut and rewrite that, so once a
``#BEGIN <id> <name> <bytes>`` line arrives for a request we are waiting on, the Framer reads
exactly ``<bytes>`` bytes, then expects ``#END <id> <crc32>``. Pure: no I/O, no clock.

A ``#BEGIN`` for no pending request (out of context, ADR-012 §6) stays an ordinary line.
"""

import zlib
from collections.abc import Callable

from esp_logcat.events import Block
from esp_logcat.parser import DumpBegin, DumpEnd, parse_line
from esp_logcat.transport import MAX_PENDING, LineSplitter

# How much of the line after the content is kept while waiting for "#END": a real one is
# 19 bytes or less, and anything longer is not it.
_END_LINE_LIMIT = 64


class Framer:
    """Same interface as LineSplitter (feed, flush, overflows), but it also yields Blocks.

    ``expects_block(id)`` says whether a request with that id is waiting for a reply: only then
    does a #BEGIN start byte counting. cancel_block() gives up on a block in progress (the
    request timed out): its bytes so far are dropped and line splitting resumes.
    """

    def __init__(
        self, expects_block: Callable[[str], bool], max_pending: int = MAX_PENDING
    ) -> None:
        self._expects_block = expects_block
        self._splitter = LineSplitter(max_pending)
        self._begin: DumpBegin | None = None  # the block in progress
        self._content = bytearray()
        self._end_line = bytearray()  # after the content: the line that should be #END
        self._cancelled = False

    @property
    def overflows(self) -> int:
        return self._splitter.overflows

    @property
    def block_id(self) -> str | None:
        """The id of the block being read, if any."""
        return self._begin.id if self._begin is not None else None

    def cancel_block(self) -> None:
        self._cancelled = True

    def feed(self, chunk: bytes) -> list[bytes | Block]:
        out: list[bytes | Block] = []
        self._apply_cancel()
        position = 0
        while position < len(chunk):
            if self._begin is None:
                position = self._feed_lines(chunk, position, out)
            elif len(self._content) < self._begin.size:
                take = min(self._begin.size - len(self._content), len(chunk) - position)
                self._content += chunk[position : position + take]
                position += take
            else:
                position = self._feed_end_line(chunk, position, out)
        return out

    def flush(self) -> list[bytes | Block]:
        """At a disconnect: a block in progress is reported incomplete, then the torn tail."""
        out: list[bytes | Block] = []
        self._apply_cancel()
        if self._begin is not None:
            out.append(self._finish(None, "connection lost inside the block"))
        out.extend(self._splitter.flush())
        return out

    def _apply_cancel(self) -> None:
        if self._cancelled:
            self._cancelled = False
            self._reset()

    def _reset(self) -> None:
        self._begin = None
        self._content.clear()
        self._end_line.clear()

    def _feed_lines(self, chunk: bytes, position: int, out: list[bytes | Block]) -> int:
        # Feed the splitter up to the next newline only: if that line opens a block, the bytes
        # after it are content and must not reach the splitter.
        newline = chunk.find(b"\n", position)
        end = len(chunk) if newline < 0 else newline + 1
        for line in self._splitter.feed(chunk[position:end]):
            begin = parse_line(line) if line.startswith(b"#BEGIN ") else None
            if isinstance(begin, DumpBegin) and self._expects_block(begin.id):
                self._begin = begin
                self._content.clear()
                self._end_line.clear()
            else:
                out.append(line)
        return end

    def _feed_end_line(self, chunk: bytes, position: int, out: list[bytes | Block]) -> int:
        newline = chunk.find(b"\n", position)
        end = len(chunk) if newline < 0 else newline + 1
        self._end_line += chunk[position:end]
        if newline < 0 and len(self._end_line) <= _END_LINE_LIMIT:
            return end  # the line is not complete yet
        line = bytes(self._end_line)
        if line.endswith(b"\r\n"):
            line = line[:-2] + b"\n"
        marker = parse_line(line) if line.endswith(b"\n") else None
        assert self._begin is not None
        if isinstance(marker, DumpEnd) and marker.id == self._begin.id:
            out.append(self._finish(marker.crc32, ""))
            return end
        # Not the #END (a "#!" inside the block, live log after a cut): the block is lost and
        # the line is ordinary data again, through the splitter like any other.
        out.append(self._finish(None, "no #END after the content"))
        out.extend(self._splitter.feed(line))
        return end

    def _finish(self, crc32: int | None, problem: str) -> Block:
        assert self._begin is not None
        content = bytes(self._content)
        if not problem and len(content) < self._begin.size:
            problem = "content cut short"
        if not problem and crc32 != zlib.crc32(content):
            problem = f"CRC mismatch: #END says {crc32:08x}, content is {zlib.crc32(content):08x}"
        block = Block(
            self._begin.id, self._begin.filename, self._begin.size, content, crc32, problem
        )
        self._reset()
        return block
