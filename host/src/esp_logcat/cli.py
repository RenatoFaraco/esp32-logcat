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
from esp_logcat.transport import (
    FileTransport,
    PortSelectionError,
    SerialTransport,
    find_port,
    list_ports,
)

# subcommand -> (help text, milestone that implements it)
SUBCOMMANDS: dict[str, tuple[str, str]] = {
    "dump": ("download logs persisted in flash", "M5"),
    "shell": ("send console commands to the device", "M5"),
}


REPLAY_HELP = """\
replay reads a saved log (from `esp-logcat stream > file`, a dump, a capture) through the same
pipeline as stream: colors, filters, gap and restart notices, summary. Files saved by
Windows PowerShell 5.1 (UTF-16) are read too.

"""

FILTER_HELP = """\
filters (like adb logcat; quote them in bash so * is not expanded):
  esp-logcat stream "*:W"                    warnings and errors only
  esp-logcat stream "*:S" "PRESSURE:D"       only PRESSURE, down to debug
  esp-logcat stream "*:W" "HEARTBEAT:I"      warnings and errors, plus the heartbeat
Without *:L every other tag stays visible (default *:V). Lines without a tag (ROM text,
invalid lines) always pass. Filtering only hides lines: gaps and the summary count all of them.
Filters and options may come in any order. Keep "LOGCAT:W" when silencing everything: it
shows the device's own loss reports.
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
    stream.set_defaults(command_parser=stream)

    replay = subparsers.add_parser(
        "replay",
        help="replay a saved log file with filters",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=REPLAY_HELP + FILTER_HELP,
    )
    replay.add_argument("file", help='log file to replay ("-" reads stdin)')
    replay.add_argument(
        "filterspecs",
        nargs="*",
        metavar="TAG:L",
        help="logcat-style filter: L is V D I W E, or S/N to silence (default *:V)",
    )
    replay.add_argument(
        "--lenient",
        action="store_true",
        help='accept "TAG:" without the space of an empty message (hand-edited files)',
    )
    replay.set_defaults(command_parser=replay)

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
    # parse_known_args: argparse cannot mix a nargs="*" positional with options (filters after
    # an option are left over), so leftovers that are not options join the filters.
    args, extras = parser.parse_known_args(argv)
    if args.command in ("stream", "replay"):
        options = [extra for extra in extras if extra.startswith("-")]
        if options:
            args.command_parser.error(f"unrecognized arguments: {' '.join(options)}")
        args.filterspecs = [*args.filterspecs, *extras]
    elif extras:
        parser.error(f"unrecognized arguments: {' '.join(extras)}")
    if args.command is None:
        parser.print_help()
        return 2
    prepare_output(sys.stdout)
    prepare_output(sys.stderr)
    if args.command in ("stream", "replay"):
        if args.command == "replay" and _file_after_filters(args):
            args.command_parser.error(
                f"{args.file} looks like a filter: the file comes first "
                "(esp-logcat replay FILE [TAG:L ...])"
            )
        try:
            args.line_filter = LogFilter.parse(args.filterspecs)
        except FilterSpecError as exc:
            args.command_parser.error(str(exc))  # exit 2, with this command's usage line
        return stream_command(args) if args.command == "stream" else replay_command(args)
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


def replay_command(args: argparse.Namespace) -> int:
    try:
        transport = FileTransport.open(args.file)
    except OSError as exc:
        message = f"cannot read {args.file}: {exc.strerror or exc}"
        if ":" in args.file and not os.path.exists(args.file) and _is_filter(args.file):
            message += " (the file comes first: esp-logcat replay FILE [TAG:L ...])"
        args.command_parser.error(message)
    try:
        code = run_stream(
            transport.chunks(),
            sys.stdout,
            sys.stderr,
            line_filter=args.line_filter,
            lenient=args.lenient,
            saved_file=True,
        )
    finally:
        transport.close()
    return 1 if transport.failed else code  # a read error must not look like a full replay


def _file_after_filters(args: argparse.Namespace) -> bool:
    """`replay "*:W" session.log`: the "file" is a filter and a filter is an existing file."""
    return (
        not os.path.exists(args.file)
        and _is_filter(args.file)
        and any(os.path.isfile(spec) for spec in args.filterspecs)
    )


def _is_filter(text: str) -> bool:
    try:
        LogFilter.parse(text)
    except FilterSpecError:
        return False
    return True


def run_stream(
    chunks: Iterable[bytes | Notice],
    out: TextIO,
    err: TextIO,
    *,
    show_time: bool = False,
    line_filter: LogFilter | None = None,
    lenient: bool = False,
    saved_file: bool = False,
) -> int:
    """Device lines go to `out`, exactly as the device wrote them (pipe and grep friendly);
    notices about the stream and the final summary go to `err`, like adb's "waiting for
    device". Stops on Ctrl+C or when the input ends.

    `line_filter` only decides what is printed: the statistics see every line, so a hidden
    tag never shows up as lost lines."""
    out_console = _console(out)
    err_console = _console(err)
    stats = StreamStats()
    parsed = events(chunks, stats, lenient=lenient, saved_file=saved_file)
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
