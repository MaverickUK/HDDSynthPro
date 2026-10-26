"""The drive's USB serial link: a background reader that keeps every line,
plus helpers to send console commands and wait for output."""
from __future__ import annotations

import os
import threading
import time

import serial
from serial.tools import list_ports

USB_PRODUCT = "HDDSynthPro"  # platformio.ini usb_product
BOARD_VIDS = (0x15BA, 0x2E8A)  # Olimex (the BB48 board definition), Raspberry Pi


def find_port() -> str | None:
    """$HDDPRO_PORT, else the first port whose USB product is HDDSynthPro.
    pyserial doesn't read the product on Windows; there, the only port with
    the board's vendor ID will do."""
    if os.environ.get("HDDPRO_PORT"):
        return os.environ["HDDPRO_PORT"]
    ports = list_ports.comports()
    for p in ports:
        if p.product == USB_PRODUCT:
            return p.device
    picos = [p for p in ports if p.vid in BOARD_VIDS and p.product is None]
    return picos[0].device if len(picos) == 1 else None


class DriveLog:
    def __init__(self, raw_log_path=None):
        self._ser: serial.Serial | None = None
        self._lines: list[str] = []
        self._last_line_at = time.monotonic()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._raw = open(raw_log_path, "a", encoding="utf-8") if raw_log_path else None
        self.ready = ""  # the board's READY line, once seen

    def open(self, timeout: float = 20.0):
        """Opens the port, waiting for it to appear (e.g. after a flash)."""
        deadline = time.monotonic() + timeout
        while True:
            port = find_port()
            if port:
                try:
                    self._ser = serial.Serial(port, 115200, timeout=0.1)
                    break
                except serial.SerialException:
                    pass
            if time.monotonic() > deadline:
                raise TimeoutError(f"no {USB_PRODUCT} serial port after {timeout:.0f} s")
            time.sleep(0.25)
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def close(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self._ser:
            self._ser.close()
        if self._raw:
            self._raw.close()

    def _reader(self):
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(4096)
            except serial.SerialException:
                break
            if not chunk:
                continue
            buf += chunk
            *complete, buf = buf.split(b"\n")
            if not complete:
                continue
            text = [c.decode("utf-8", "replace").rstrip("\r") for c in complete]
            with self._lock:
                self._lines.extend(text)
                self._last_line_at = time.monotonic()
            if self._raw:
                self._raw.write("\n".join(text) + "\n")
                self._raw.flush()

    def send(self, command: str):
        self._ser.write(command.encode())

    def mark(self) -> int:
        """A position in the log; pass to lines_since() / wait_for()."""
        with self._lock:
            return len(self._lines)

    def lines_since(self, mark: int) -> list[str]:
        with self._lock:
            return self._lines[mark:]

    def wait_for(self, predicate, since: int, timeout: float) -> str:
        """Waits for a line after `since` that satisfies `predicate`."""
        deadline = time.monotonic() + timeout
        seen = since
        while True:
            with self._lock:
                new = self._lines[seen:]
                seen = len(self._lines)
            for line in new:
                if predicate(line):
                    return line
            if time.monotonic() > deadline:
                raise TimeoutError(f"no matching line within {timeout:.0f} s")
            time.sleep(0.05)

    def wait_quiet(self, quiet: float, timeout: float):
        """Waits until the drive has printed nothing for `quiet` seconds."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                last = self._last_line_at
            if time.monotonic() - last >= quiet:
                return
            time.sleep(0.1)
        raise TimeoutError(f"trace still busy after {timeout:.0f} s")

    def command(self, command: str, reply_prefix: str, timeout: float = 3.0) -> str:
        """Sends a console command and returns the first line starting with reply_prefix."""
        mark = self.mark()
        self.send(command)
        return self.wait_for(lambda l: l.startswith(reply_prefix), mark, timeout)
