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
  2. HDDPRO_REPLUG=redetect (drive build only): no replug at all. The drive
     leaves the bus (console `V`) and a read is sent to it; when the read
     fails the adapter resets and probes for a drive (its first RESET# pulse
     comes about 20 s later), and the drive comes back, so the adapter
     detects it again: IDENTIFY, SET FEATURES, then the read is retried. The
     drive comes back at the first RESET# assert, so it's there before the
     reset ends, as a drive powering up with the adapter would be. The
     host keeps the disk. It's a re-detection after a lost drive, not the
     power-up probe a real replug gives (slice 0 needs one of those).
  3. Otherwise the test asks you to do it by hand and waits.
"""
from __future__ import annotations

import os
import shlex
import subprocess
import sys
import threading
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
        self._redetect = os.environ.get("HDDPRO_REPLUG", "").strip().lower() == "redetect"
        self.drive = None  # the DriveLog, for redetect (conftest's `rig` sets it)

    @property
    def automatic_replug(self) -> bool:
        return bool(self._uhubctl) or self._redetect

    def replug(self):
        """Disconnects and reconnects the adapter. Manual mode returns at once;
        the caller waits for the drive to see bus activity."""
        if self._redetect:
            self.redetect()
        elif self._uhubctl:
            targets = [shlex.split(t) for t in self._uhubctl.split(";") if t.strip()]
            for target in targets:
                subprocess.run(["uhubctl", "-a", "off", *target], check=True, capture_output=True)
            time.sleep(float(os.environ.get("HDDPRO_REPLUG_OFF_S", "2")))
            for target in targets:
                subprocess.run(["uhubctl", "-a", "on", *target], check=True, capture_output=True)
        else:
            self._ask("Replug the USB-IDE adapter now: unplug its USB cable and plug it back in "
                      "(the BB48 stays powered).")

    REDETECT_RESET_TIMEOUT_S = 110  # under the firmware's 120 s, after which it comes back anyway

    REDETECT_DETECT_TIMEOUT_S = 60

    def redetect(self):
        """Makes the adapter detect the drive again without a replug (see the
        module's docstring). Returns once the adapter has sent IDENTIFY and
        the failed read has been retried, so the trace since the call holds
        the whole detection."""
        drive = self.drive
        if drive is None:
            raise RuntimeError("redetect needs the drive's serial log (Host.drive)")
        disk = self.find_disk(timeout=30)
        mark = drive.mark()
        try:
            reply = drive.command("V", "ABSENT", timeout=3)
        except TimeoutError:
            raise RuntimeError("no reply to console V: redetect needs the drive build") from None
        if reply != "ABSENT on":
            raise RuntimeError(f"the drive didn't leave the bus: {reply}")
        try:
            # The read the adapter fails on. It's retried and succeeds once the
            # drive is back, so the thread ends by itself.
            fd = self.open_raw(disk)

            def read():
                try:
                    pread(fd, 512, 0)
                except OSError:
                    pass
                finally:
                    os.close(fd)

            reader = threading.Thread(target=read, daemon=True)
            reader.start()
            # The drive comes back by itself at the adapter's first RESET#.
            drive.wait_for(lambda l: l.startswith("ABSENT off"), since=mark,
                           timeout=self.REDETECT_RESET_TIMEOUT_S)
        except BaseException:
            drive.command("V", "ABSENT off", timeout=3)  # don't leave it gone
            raise
        drive.wait_for(lambda l: l.startswith("CMD") and l.split()[2] == "ec", since=mark,
                       timeout=self.REDETECT_DETECT_TIMEOUT_S)
        reader.join(timeout=self.REDETECT_DETECT_TIMEOUT_S)

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
