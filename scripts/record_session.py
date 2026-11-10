"""Record a real `esp-logcat stream` session from the board (for the manual's screenshots).

Usage (from the repository root, with the host venv):
    host/.venv/Scripts/python scripts/record_session.py <port> <seconds> <out.json> [--reset]

Stores exactly what SerialTransport yielded (byte chunks and notices) with their time offsets.
scripts/manual_screenshots.py replays it through the same pipeline and renderer as the real
command, so the pictures in docs/project/MANUAL.md show real device output and can be regenerated.
"""

import base64
import json
import sys
import time
from datetime import datetime
from pathlib import Path

from esp_logcat.events import Notice
from esp_logcat.transport import SerialTransport


def main() -> int:
    args = [arg for arg in sys.argv[1:] if not arg.startswith("--")]
    if len(args) != 3:
        print(__doc__)
        return 2
    port, seconds, out = args[0], float(args[1]), Path(args[2])
    transport = SerialTransport(port, reset="--reset" in sys.argv)
    started = datetime.now()
    start = time.monotonic()
    items: list[dict[str, object]] = []
    for item in transport.chunks():
        offset = round(time.monotonic() - start, 4)
        if isinstance(item, Notice):
            items.append({"t": offset, "notice": item.kind.value, "text": item.text})
        else:
            items.append({"t": offset, "data": base64.b64encode(item).decode("ascii")})
        if offset > seconds:
            transport.close()
    session = {"port": port, "started": started.isoformat(timespec="milliseconds"), "items": items}
    out.write_text(json.dumps(session, indent=1) + "\n", encoding="utf-8")
    print(f"{out}: {len(items)} items")
    return 0


if __name__ == "__main__":
    sys.exit(main())
