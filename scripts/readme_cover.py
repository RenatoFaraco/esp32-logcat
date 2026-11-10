"""Draw the README cover (docs/project/img/cover.svg): project, board, real logcat output, stacks.

Usage (from the repository root, with the host venv):
    host/.venv/Scripts/python scripts/readme_cover.py

The terminal in the picture is real board output: a session recorded by
scripts/record_session.py, rendered by the same code as `esp-logcat stream` (see
scripts/manual_screenshots.py). It is inlined as a nested <svg>, because GitHub shows README
images as <img>, which never loads files referenced from inside an SVG.
"""

import io
import re
import sys
from pathlib import Path
from xml.sax.saxutils import escape

from rich.console import Console
from rich.terminal_theme import MONOKAI
from rich.text import Text

sys.path.insert(0, str(Path(__file__).resolve().parent))
import manual_screenshots as shots  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "project" / "img" / "cover.svg"
WIDTH, HEIGHT = 1280, 640
SANS = "Segoe UI, Helvetica, Arial, sans-serif"
MONO = "Cascadia Code, Consolas, Menlo, monospace"

STACKS = [
    "ESP32-C3 · RISC-V",
    "ESP-IDF 6.1",
    "FreeRTOS",
    "C++17",
    "PlatformIO",
    "Unity",
    "Python 3.12",
    "pyserial",
    "rich",
    "pytest",
    "GitHub Actions",
]


def terminal_svg(x: int, y: int, width: int) -> str:
    """Real `esp-logcat stream` output (the burst and the heartbeat) as a nested <svg>."""
    # Indices of the rendered lines of the "boot" session: the first lines after the hook, the
    # burst's gaps with the firmware's own report, and the heartbeat.
    all_lines = shots.stream_lines(shots.Shot("", "", "", "boot", summary=False))
    lines = all_lines[57:62] + [Text("...", style="dim")] + all_lines[87:102]
    console = Console(
        file=io.StringIO(),
        record=True,
        width=104,
        force_terminal=True,
        color_system="truecolor",
        highlight=False,
        emoji=False,
    )
    prompt = Text("PS C:\\dev\\esp32-logcat> ", style="bold blue")
    prompt.append("esp-logcat stream --reset", style="bold")
    console.print(prompt)
    for line in lines:
        console.print(line, no_wrap=True, overflow="ellipsis")
    svg = console.export_svg(title="esp-logcat stream", theme=MONOKAI)
    view_w, view_h = map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', svg).groups())
    height = round(width * view_h / view_w)
    svg = svg.replace("<svg ", f'<svg x="{x}" y="{y}" width="{width}" height="{height}" ', 1)
    return svg


def board_svg(x: int, y: int) -> str:
    """Stylized drawing of the board: ESP32-C3 module, CH340K bridge, two USB-C ports."""
    parts = [f'<g transform="translate({x},{y})">']
    # PCB
    parts.append('<rect x="40" y="0" width="300" height="150" rx="12" fill="#1d3557"/>')
    parts.append(
        '<rect x="40" y="0" width="300" height="150" rx="12" fill="none" '
        'stroke="#457b9d" stroke-width="2"/>'
    )
    # Pin headers along both long edges
    for i in range(15):
        cx = 70 + i * 18
        for cy in (14, 136):
            parts.append(f'<circle cx="{cx}" cy="{cy}" r="4.5" fill="#e9c46a"/>')
            parts.append(f'<circle cx="{cx}" cy="{cy}" r="2" fill="#1d3557"/>')
    # Two USB-C receptacles on the left edge
    for cy, label in ((48, "USB-Serial-JTAG"), (102, "CH340K · UART0")):
        parts.append(
            f'<rect x="22" y="{cy - 12}" width="40" height="24" rx="10" '
            'fill="#c0c7cf" stroke="#8d99a6" stroke-width="1.5"/>'
        )
        parts.append(f'<rect x="30" y="{cy - 4}" width="24" height="8" rx="4" fill="#5c6670"/>')
        parts.append(
            f'<text x="14" y="{cy + 4}" text-anchor="end" font-family="{SANS}" '
            f'font-size="13" fill="#c9d1d9">{label}</text>'
        )
    # ESP32-C3 chip
    parts.append('<rect x="180" y="40" width="70" height="70" rx="5" fill="#111"/>')
    parts.append(
        '<rect x="186" y="46" width="58" height="58" rx="3" fill="none" '
        'stroke="#444" stroke-width="1"/>'
    )
    parts.append(
        f'<text x="215" y="72" text-anchor="middle" font-family="{SANS}" '
        'font-size="11" font-weight="600" fill="#e6edf3">ESP32</text>'
    )
    parts.append(
        f'<text x="215" y="88" text-anchor="middle" font-family="{SANS}" '
        'font-size="11" font-weight="600" fill="#e6edf3">-C3</text>'
    )
    # CH340K bridge chip
    parts.append('<rect x="100" y="88" width="34" height="26" rx="3" fill="#111"/>')
    parts.append(
        f'<text x="117" y="105" text-anchor="middle" font-family="{SANS}" '
        'font-size="7.5" fill="#c9d1d9">CH340K</text>'
    )
    # Buttons and LED
    for bx, label in ((278, "BOOT"), (278 + 36, "RST")):
        parts.append(f'<rect x="{bx - 12}" y="42" width="24" height="24" rx="3" fill="#ddd"/>')
        parts.append(f'<circle cx="{bx}" cy="54" r="6" fill="#666"/>')
        parts.append(
            f'<text x="{bx}" y="80" text-anchor="middle" font-family="{SANS}" '
            f'font-size="9" fill="#c9d1d9">{label}</text>'
        )
    parts.append('<circle cx="296" cy="108" r="5" fill="#2ec4b6"/>')
    parts.append('<rect x="100" y="40" width="40" height="30" rx="3" fill="#2b2d42"/>')
    parts.append(
        f'<text x="120" y="59" text-anchor="middle" font-family="{SANS}" '
        'font-size="7.5" fill="#c9d1d9">4 MB</text>'
    )
    parts.append("</g>")
    parts.append(
        f'<text x="{x + 190}" y="{y + 178}" text-anchor="middle" font-family="{SANS}" '
        'font-size="15" fill="#8b949e">ESP32-C3 rev v0.4 · RISC-V 160 MHz · '
        "4 MB flash · 2× USB-C</text>"
    )
    return "\n".join(parts)


def stacks_svg(y: int) -> str:
    parts = []
    widths = [round(len(name) * 7.4 + 26) for name in STACKS]
    gap = 9
    x = (WIDTH - (sum(widths) + gap * (len(STACKS) - 1))) / 2
    for name, w in zip(STACKS, widths, strict=True):
        parts.append(
            f'<rect x="{x:.0f}" y="{y}" width="{w}" height="32" rx="16" '
            'fill="#21262d" stroke="#30363d"/>'
        )
        parts.append(
            f'<text x="{x + w / 2:.0f}" y="{y + 21}" text-anchor="middle" '
            f'font-family="{SANS}" font-size="14" fill="#c9d1d9">{escape(name)}</text>'
        )
        x += w + gap
    return "\n".join(parts)


def main() -> None:
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" '
        f'viewBox="0 0 {WIDTH} {HEIGHT}">',
        "<title>esp32-logcat</title>",
        '<defs><linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#0d1117"/><stop offset="1" stop-color="#161b22"/>'
        "</linearGradient></defs>",
        f'<rect width="{WIDTH}" height="{HEIGHT}" rx="18" fill="url(#bg)"/>',
        f'<text x="60" y="108" font-family="{SANS}" font-size="58" font-weight="700" '
        'fill="#e6edf3">esp32-logcat</text>',
        f'<text x="62" y="150" font-family="{SANS}" font-size="22" fill="#8b949e">'
        "Persistent, adb logcat-style logging</text>",
        f'<text x="62" y="180" font-family="{SANS}" font-size="22" fill="#8b949e">'
        "for ESP32 firmware</text>",
        f'<text x="62" y="222" font-family="{MONO}" font-size="15" fill="#7ee787">'
        "every ESP_LOGx · never blocks · numbered lines</text>",
        board_svg(110, 300),
        terminal_svg(500, 40, 740),
        f'<text x="500" y="412" font-family="{SANS}" font-size="16" fill="#8b949e">'
        "Real output from the board: a burst of 100 lines fills the 32-line queue.</text>",
        f'<text x="500" y="436" font-family="{SANS}" font-size="16" fill="#8b949e">'
        "Lost lines leave a gap in seq, are counted and reported, and the caller never waits."
        "</text>",
        stacks_svg(570),
        "</svg>",
    ]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(svg) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
