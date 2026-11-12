"""Record real `esp-logcat shell` runs from the board (for the manual's pictures).

Usage (from the repository root, with the host venv, the board on its native port):
    host/.venv/Scripts/python scripts/record_shell.py <port> docs/project/manual/sessions/shell.json

Each run opens the port as the shell does and sends its commands through the real Shell; what
the transport yielded (byte chunks, notices) and what the shell wrote are stored with their
time offsets. scripts/manual_screenshots.py replays them through the shell's own backend
(events, Exchange, print_reply), so the pictures show real replies and can be regenerated.
"""

import base64
import io
import json
import sys
import time
from datetime import datetime
from pathlib import Path

from esp_logcat.events import Notice
from esp_logcat.filters import LogFilter
from esp_logcat.shell import Shell, handle, run_one
from esp_logcat.transport import SerialTransport
from rich.console import Console

# name -> how to run it. "one": `esp-logcat shell <command>`; "prompt": the interactive shell,
# with (command, seconds to wait afterwards) pairs; "" just waits (log lines arrive meanwhile).
RUNS: dict[str, dict[str, object]] = {
    "info": {"mode": "one", "commands": ["info"]},
    "level": {"mode": "one", "commands": ["log level HEARTBEAT D"]},
    "bad_args": {"mode": "one", "commands": ["log level APP x"]},
    "tail": {"mode": "one", "commands": ["log tail"]},
    "json": {"mode": "one", "commands": ["stats"]},
    "prompt": {
        "mode": "prompt",
        "filters": ["*:S", "HEARTBEAT:V"],
        # The demo app's load phases log 1000 lines/s under LOAD: silenced on the device for this
        # run (outside the recording), so the file stays small. The device filter drops a line
        # before it takes a seq, so no gap appears.
        # HEARTBEAT starts at I, so the D lines appear only after the command that opens them.
        "before": ["log level LOAD N", "log level HEARTBEAT I"],
        "after": ["log level LOAD I"],
        "commands": [
            ["", 2.2],
            ["stats", 1.2],
            ["log level HEARTBEAT D", 2.2],
            ["log level HEARTBEAT I", 1.2],
        ],
    },
}


class Recording:
    """Wraps the transport: what it yields and what is written to it, with time offsets."""

    def __init__(self, transport: SerialTransport) -> None:
        self._transport = transport
        self._start = time.monotonic()
        self.items: list[dict[str, object]] = []

    def _offset(self) -> float:
        return round(time.monotonic() - self._start, 4)

    def chunks(self):
        for item in self._transport.chunks():
            if isinstance(item, Notice):
                self.items.append(
                    {"t": self._offset(), "notice": item.kind.value, "text": item.text}
                )
            else:
                data = base64.b64encode(item).decode("ascii")
                self.items.append({"t": self._offset(), "data": data})
            yield item

    def write(self, data: bytes) -> bool:
        self.items.append({"t": self._offset(), "write": data.decode("ascii")})
        return self._transport.write(data)

    def close(self) -> None:
        self._transport.close()


def send_quietly(port: str, command: str) -> None:
    """One command outside any recording."""
    quiet = Console(file=io.StringIO())
    shell = Shell(SerialTransport(port), quiet, quiet)
    shell.start()
    try:
        run_one(shell, command.split(), quiet, quiet, as_json=False)
    finally:
        shell.close()
    time.sleep(0.5)


def record(port: str, spec: dict[str, object]) -> dict[str, object]:
    for command in spec.get("before", []):  # type: ignore[union-attr]
        send_quietly(port, command)
    try:
        return _record(port, spec)
    finally:
        for command in spec.get("after", []):  # type: ignore[union-attr]
            send_quietly(port, command)


def _record(port: str, spec: dict[str, object]) -> dict[str, object]:
    recording = Recording(SerialTransport(port))
    started = datetime.now()
    quiet = Console(file=io.StringIO())
    filters = list(spec.get("filters", []))  # type: ignore[arg-type]
    shell = Shell(
        recording,
        quiet,
        quiet,
        show_logs=spec["mode"] == "prompt",
        line_filter=LogFilter.parse(filters) if filters else None,
    )
    shell.start()
    try:
        if spec["mode"] == "one":
            [command] = spec["commands"]  # type: ignore[misc]
            run_one(shell, command.split(), quiet, quiet, as_json=False)
        else:
            shell.wait_connected(3.0)
            for command, pause in spec["commands"]:  # type: ignore[union-attr]
                if command:
                    handle(shell, command, quiet, quiet)
                time.sleep(pause)
    finally:
        shell.close()
    return {
        **spec,
        "started": started.isoformat(timespec="milliseconds"),
        "items": recording.items,
    }


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    port, out = sys.argv[1], Path(sys.argv[2])
    runs = {}
    for name, spec in RUNS.items():
        runs[name] = record(port, spec)
        print(f"{name}: {len(runs[name]['items'])} items")
        time.sleep(0.5)  # let the port settle before the next run opens it
    out.write_text(json.dumps(runs, indent=1) + "\n", encoding="utf-8")
    print(f"{out}: {len(runs)} runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
