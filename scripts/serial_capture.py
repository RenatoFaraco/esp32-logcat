"""Reset the ESP32-C3 and capture its boot output on both USB ports at once.

Usage (from the repository root, with the host venv):
    host/.venv/Scripts/python scripts/serial_capture.py <uart_port> <usb_jtag_port> <seconds> <prefix>

Writes <prefix>_<port>.log for each port. The reset goes through the native USB-Serial-JTAG,
which restarts the CPU without dropping the USB link, so both ports stay open during the boot.
See docs/context/hardware.md for the port behaviors this relies on.
"""

import sys
import threading
import time

import serial


def open_port(name: str) -> serial.Serial:
    # DTR/RTS released before opening: on the CH340K bridge RTS drives EN (reset) and DTR drives
    # GPIO9 (download mode), so active lines would hold the chip in reset or in the ROM loader.
    link = serial.Serial()
    link.port = name
    link.baudrate = 115200
    link.timeout = 0.05
    link.dtr = False
    link.rts = False
    link.open()
    return link


def set_rts(link: serial.Serial, state: bool) -> None:
    # Windows' usbser.sys only sends SET_CONTROL_LINE_STATE when DTR changes: write DTR again
    # (same value) after each RTS change so the device sees it. esptool does the same.
    link.rts = state
    link.dtr = link.dtr


def reset_via_usb_jtag(link: serial.Serial) -> None:
    link.dtr = False
    set_rts(link, True)
    time.sleep(0.2)
    set_rts(link, False)


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    uart_name, jtag_name, seconds, prefix = (
        sys.argv[1],
        sys.argv[2],
        float(sys.argv[3]),
        sys.argv[4],
    )

    links = [open_port(uart_name), open_port(jtag_name)]
    captured = {link.port: bytearray() for link in links}
    errors: list[str] = []
    deadline = time.monotonic() + seconds

    def reader(link: serial.Serial) -> None:
        while time.monotonic() < deadline:
            try:
                captured[link.port] += link.read(4096)
            except serial.SerialException as exc:  # a USB port may re-enumerate
                errors.append(f"{link.port}: {exc}")
                return

    threads = [threading.Thread(target=reader, args=(link,)) for link in links]
    for thread in threads:
        thread.start()
    time.sleep(0.3)
    reset_via_usb_jtag(links[1])
    for thread in threads:
        thread.join()
    for link in links:
        try:
            link.close()
        except serial.SerialException:
            pass

    for name, data in captured.items():
        with open(f"{prefix}_{name}.log", "wb") as file:
            file.write(data)
        print(f"{name}: {len(data)} bytes")
    for error in errors:
        print("error:", error)
    return 0 if all(captured.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
