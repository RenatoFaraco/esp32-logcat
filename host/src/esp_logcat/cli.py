"""Command-line entry point. Subcommands not implemented yet name their milestone."""

import argparse
import contextlib
import errno
import os
import sys
from collections.abc import Iterable, Sequence
from typing import TextIO

from rich.console import Console

from esp_logcat import __version__
from esp_logcat.events import Notice, Received
from esp_logcat.filters import FilterSpecError, LogFilter
from esp_logcat.pipeline import events, visible
from esp_logcat.render import device_text, render, render_notice
from esp_logcat.stats import StreamStats
from esp_logcat.transport import PortSelectionError, SerialTransport, find_port, list_ports

# subcommand -> (help text, milestone that implements it)
SUBCOMMANDS: dict[str, tuple[str, str]] = {
    "dump": ("download logs persisted in flash", "M5"),
    "shell": ("send console commands to the device", "M5"),
    "replay": ("replay a saved log file with filters", "M3"),
}


FILTER_HELP = """\
filters (like adb logcat; quote them in bash so * is not expanded):
  esp-logcat stream "*:W"                    warnings and errors only
  esp-logcat stream "*:S" "PRESSURE:D"       only PRESSURE, down to debug
  esp-logcat stream "*:W" "HEARTBEAT:I"      warnings and errors, plus the heartbeat
Without *:L every other tag stays visible (default *:V). Lines without a tag (ROM text,
invalid lines) always pass. Filtering only hides lines: gaps and the summary count all of them.
Keep the filters together (options before or after them, not in between). Keep "LOGCAT:W" when
silencing everything: it shows the device's own loss reports.
"""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="esp-logcat", description=__doc__)
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    subparsers = parser.add_subparsers(dest="command", metavar="<command>")

    stream = subparsers.add_parser(
        "stream",
        help="stream live logs from the device",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=FILTER_HELP,
    )
    stream.add_argument(
        "filterspecs",
        nargs="*",
        metavar="TAG:L",
        help="logcat-style filter: L is V D I W E, or S/N to silence (default *:V)",
    )
    stream.add_argument("--port", help="serial port (default: the native USB port, if unique)")
    stream.add_argument(
        "--baud", type=_positive_int, default=115200, help="baud rate (default: 115200)"
    )
    stream.add_argument("--reset", action="store_true", help="reset the device after opening")
    stream.add_argument("--time", action="store_true", help="add the PC time to each line")
    stream.set_defaults(stream_parser=stream)

    subparsers.add_parser("ports", help="list serial ports")

    for name, (help_text, _) in SUBCOMMANDS.items():
        subparsers.add_parser(name, help=help_text)
    return parser


def _positive_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError:
        number = 0
    if number <= 0:
        raise argparse.ArgumentTypeError(f"must be a positive integer: {value!r}")
    return number


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return 2
    prepare_output(sys.stdout)
    prepare_output(sys.stderr)
    if args.command == "stream":
        try:
            args.line_filter = LogFilter.parse(args.filterspecs)
        except FilterSpecError as exc:
            args.stream_parser.error(str(exc))  # exit 2, with the stream usage line
        return stream_command(args)
    if args.command == "ports":
        return ports_command()
    milestone = SUBCOMMANDS[args.command][1]
    print(f"esp-logcat {args.command}: not implemented yet ({milestone})", file=sys.stderr)
    return 1


def prepare_output(stream: TextIO) -> None:
    """Never die on text the console encoding lacks: a piped stdout is cp1252 on Windows and
    cannot write the U+FFFD the parser leaves for invalid UTF-8 from the device."""
    reconfigure = getattr(stream, "reconfigure", None)
    if reconfigure is not None:
        reconfigure(errors="replace")


def stream_command(args: argparse.Namespace) -> int:
    err = _console(sys.stderr)
    try:
        port = args.port or find_port(list_ports())
    except PortSelectionError as exc:
        err.print(f"esp-logcat: {exc}", markup=False)
        return 2
    transport = SerialTransport(port, args.baud, reset=args.reset)
    try:
        return run_stream(
            transport.chunks(),
            sys.stdout,
            sys.stderr,
            show_time=args.time,
            line_filter=args.line_filter,
        )
    finally:
        transport.close()


def run_stream(
    chunks: Iterable[bytes | Notice],
    out: TextIO,
    err: TextIO,
    *,
    show_time: bool = False,
    line_filter: LogFilter | None = None,
) -> int:
    """Device lines go to `out`, exactly as the device wrote them (pipe and grep friendly);
    notices about the stream and the final summary go to `err`, like adb's "waiting for
    device". Stops on Ctrl+C or when the input ends.

    `line_filter` only decides what is printed: the statistics see every line, so a hidden
    tag never shows up as lost lines."""
    out_console = _console(out)
    err_console = _console(err)
    stats = StreamStats()
    parsed = events(chunks, stats)
    source = visible(parsed, stats, line_filter)
    try:
        for event in source:
            match event:
                case Received():
                    pc_time = event.pc_time if show_time else None
                    out_console.print(render(event.line, pc_time=pc_time))
                case Notice():
                    err_console.print(render_notice(event))
    except KeyboardInterrupt:
        pass
    except OSError as exc:  # "esp-logcat stream | head": the reader went away
        if not _is_closed_pipe(exc):
            raise
        _discard_further_output(out)
    finally:
        # Close the generators now, not at garbage collection: closing the transport's
        # generator closes the serial port.
        source.close()
        parsed.close()
        close_chunks = getattr(chunks, "close", None)
        if close_chunks is not None:
            close_chunks()
        err_console.print(stats.summary(), markup=False, style="dim")
    return 0


def _is_closed_pipe(exc: OSError) -> bool:
    # POSIX raises BrokenPipeError; Windows raises OSError EINVAL on a closed pipe.
    return isinstance(exc, BrokenPipeError) or exc.errno == errno.EINVAL


def _discard_further_output(stream: TextIO) -> None:
    # The interpreter flushes stdout at exit and would fail again with a traceback: point
    # the file descriptor at devnull (what the Python docs recommend for SIGPIPE).
    with contextlib.suppress(OSError, ValueError, AttributeError):
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, stream.fileno())


def ports_command() -> int:
    out = _console(sys.stdout)
    ports = list_ports()
    if not ports:
        out.print("no serial ports found", markup=False)
    for port in ports:
        ids = f"{port.vid:04X}:{port.pid:04X}" if port.vid is not None else "-"
        kind = "native USB-Serial-JTAG" if port.is_native else ""
        serial_number = f"SER={port.serial_number}" if port.serial_number else ""
        fields = [port.device, ids, serial_number, kind, port.description]
        # The description comes from the USB device (iProduct on Linux/macOS): escape it.
        out.print(device_text("  ".join(field for field in fields if field)))
    return 0


class _Console(Console):
    def on_broken_pipe(self) -> None:
        # rich's default redirects sys.stdout (whatever file this console writes to) and exits
        # with status 1; let run_stream end the stream and print the summary instead.
        raise BrokenPipeError(errno.EPIPE, "broken pipe")


def _console(file: TextIO) -> Console:
    # soft_wrap: never wrap at the console width (off a terminal rich assumes 80 columns).
    # color_system stays "auto": an explicit one forces ANSI codes even into a pipe.
    return _Console(file=file, soft_wrap=True, highlight=False, emoji=False)
