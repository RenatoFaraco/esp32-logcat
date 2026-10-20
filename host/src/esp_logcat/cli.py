"""Command-line entry point. Subcommands are placeholders until their milestones land."""

import argparse
import sys
from collections.abc import Sequence

from esp_logcat import __version__

# subcommand -> (help text, milestone that implements it)
SUBCOMMANDS: dict[str, tuple[str, str]] = {
    "stream": ("stream live logs from the device", "M2"),
    "dump": ("download logs persisted in flash", "M5"),
    "shell": ("send console commands to the device", "M5"),
    "replay": ("replay a saved log file with filters", "M3"),
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="esp-logcat", description=__doc__)
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    subparsers = parser.add_subparsers(dest="command", metavar="<command>")
    for name, (help_text, _) in SUBCOMMANDS.items():
        subparsers.add_parser(name, help=help_text)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return 2
    milestone = SUBCOMMANDS[args.command][1]
    print(f"esp-logcat {args.command}: not implemented yet ({milestone})", file=sys.stderr)
    return 1
