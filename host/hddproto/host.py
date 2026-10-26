"""The host side of the rig, whatever the OS: replugging the USB-IDE adapter,
finding the emulated drive as a block device, reading it raw, and the system
log for the failure folder. `make_host()` picks the class for this OS; the
OS-specific parts live in host_macos.py and host_windows.py.

Replug, in order of preference:
  1. HDDPRO_UHUBCTL set: uhubctl switches the adapter's hub port off, waits
     HDDPRO_REPLUG_OFF_S (default 2 s) and switches it back on. Its value is
     one or more uhubctl targets separated by ";". A USB 3 port needs two:
     the USB 3 half and its USB 2 companion. For the rig's Realtek hub:
       HDDPRO_UHUBCTL="-l 0-2 -e -p 4;-l 0-1 -e -p 4"
     (-e: exact location. Without it, uhubctl on macOS finds no hub.)
  2. Otherwise the test asks you to do it by hand and waits.
"""
from __future__ import annotations

import os
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass


@dataclass
class Disk:
    name: str          # e.g. "disk4" (macOS), "PhysicalDrive3" (Windows)
    media_name: str    # the product half of the ATA model only, e.g. "PROTO"
    size: int          # bytes
    # The full name, e.g. "HDDSYNTH PROTO Media". The NS1081 splits the ATA
    # model at its first space into the SCSI vendor ("HDDSYNTH", 8 chars) and
    # product ("PROTO"); diskutil's MediaName keeps only the product.
    full_name: str = ''
    raw: str = ''      # the uncached device to open, e.g. /dev/rdisk4


class Host:
    def __init__(self, ask):
        """`ask(message)` shows a message to the person at the rig (bypassing
        pytest's output capture)."""
        self._ask = ask
        self._uhubctl = os.environ.get("HDDPRO_UHUBCTL")

    @property
    def automatic_replug(self) -> bool:
        return bool(self._uhubctl)

    def replug(self):
        """Disconnects and reconnects the adapter. Manual mode returns at once;
        the caller waits for the drive to see bus activity."""
        if self._uhubctl:
            targets = [shlex.split(t) for t in self._uhubctl.split(";") if t.strip()]
            for target in targets:
                subprocess.run(["uhubctl", "-a", "off", *target], check=True, capture_output=True)
            time.sleep(float(os.environ.get("HDDPRO_REPLUG_OFF_S", "2")))
            for target in targets:
                subprocess.run(["uhubctl", "-a", "on", *target], check=True, capture_output=True)
        else:
            self._ask("Replug the USB-IDE adapter now: unplug its USB cable and plug it back in "
                      "(the BB48 stays powered).")

    def find_disk(self, expected_bytes: int | None = None, timeout: float = 60.0) -> Disk:
        """The emulated drive: $HDDPRO_DISK (e.g. disk4, PhysicalDrive3), or
        the external disk whose name says HDDSYNTH or whose size matches."""
        wanted = os.environ.get("HDDPRO_DISK", "").removeprefix("/dev/").removeprefix("\\\\.\\")
        deadline = time.monotonic() + timeout
        while True:
            for disk in self._candidates():
                if wanted:
                    if disk.name == wanted:
                        return disk
                elif "HDDSYNTH" in disk.full_name.upper() or (expected_bytes and disk.size == expected_bytes):
                    return disk
            if time.monotonic() > deadline:
                raise TimeoutError(f"no HDDSynthPro disk after {timeout:.0f} s:\n{self.disks()}")
            time.sleep(1)

    # -- For each OS ------------------------------------------------------------

    def _candidates(self) -> list[Disk]:
        """The external disks attached now."""
        raise NotImplementedError

    def open_raw(self, disk: Disk, write: bool = False) -> int:
        """Opens the disk's raw device; returns a file descriptor."""
        raise NotImplementedError

    def system_log(self, last: str = "3m") -> str:
        """Kernel messages about USB and disks over the last minutes ("5m")."""
        raise NotImplementedError

    def disks(self) -> str:
        """The external disks, as text for a failure message."""
        raise NotImplementedError


def make_host(ask) -> Host:
    if sys.platform == "darwin":
        from hddproto.host_macos import MacHost
        return MacHost(ask)
    if sys.platform == "win32":
        from hddproto.host_windows import WindowsHost
        return WindowsHost(ask)
    raise NotImplementedError(f"no rig host for {sys.platform} yet (slice 6 adds Linux)")


# Positional reads and writes on the raw device. Windows has no os.pread or
# os.pwrite; a seek then a read does the same, as long as one thread at a
# time uses the descriptor (the tests do).

def pread(fd: int, length: int, offset: int) -> bytes:
    if hasattr(os, "pread"):
        return os.pread(fd, length, offset)
    os.lseek(fd, offset, os.SEEK_SET)
    return os.read(fd, length)


def pwrite(fd: int, data: bytes, offset: int) -> int:
    if hasattr(os, "pwrite"):
        return os.pwrite(fd, data, offset)
    os.lseek(fd, offset, os.SEEK_SET)
    return os.write(fd, data)
