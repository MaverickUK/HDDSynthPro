"""The macOS side of the rig: replugging the USB-IDE adapter and capturing
the system log. Everything specific to the host OS lives here, so a Linux
version (needed for ATA pass-through in slice 6) can sit alongside it.

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
import plistlib
import shlex
import subprocess
import time
from dataclasses import dataclass


class MacHost:
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
                      "(or switch its power brick off and on; the BB48 stays powered).")

    def system_log(self, last: str = "3m") -> str:
        """Kernel messages about USB and disks, for the failure folder."""
        predicate = ('process == "kernel" AND (eventMessage CONTAINS[c] "usb" OR '
                     'eventMessage CONTAINS[c] "disk" OR eventMessage CONTAINS[c] "scsi")')
        result = subprocess.run(["log", "show", "--last", last, "--style", "compact",
                                 "--predicate", predicate], capture_output=True, text=True)
        return result.stdout + result.stderr

    def disks(self) -> str:
        return subprocess.run(["diskutil", "list", "external", "physical"],
                              capture_output=True, text=True).stdout

    # -- The drive as a block device ------------------------------------------
    #
    # macOS has no O_DIRECT; the raw device /dev/rdiskN is uncached instead.
    # It needs sector-aligned offsets and lengths. A disk with no file system
    # (the generated disk) makes macOS show "not readable": choose Ignore.

    def find_disk(self, expected_bytes: int | None = None, timeout: float = 60.0) -> Disk:
        """The emulated drive: $HDDPRO_DISK (e.g. disk4), or the external
        physical disk whose media name says HDDSYNTH or whose size matches."""
        deadline = time.monotonic() + timeout
        while True:
            for disk in self._candidates():
                if os.environ.get("HDDPRO_DISK"):
                    if disk.name == os.environ["HDDPRO_DISK"].removeprefix("/dev/"):
                        return disk
                elif "HDDSYNTH" in disk.media_name.upper() or (expected_bytes and disk.size == expected_bytes):
                    return disk
            if time.monotonic() > deadline:
                raise TimeoutError(f"no HDDSynthPro disk after {timeout:.0f} s:\n{self.disks()}")
            time.sleep(1)

    def _candidates(self) -> list[Disk]:
        out = subprocess.run(["diskutil", "list", "-plist", "external", "physical"],
                             capture_output=True).stdout
        if not out:
            return []
        disks = []
        for name in plistlib.loads(out).get("WholeDisks", []):
            info = plistlib.loads(subprocess.run(["diskutil", "info", "-plist", name],
                                                 capture_output=True).stdout)
            disks.append(Disk(name, info.get("MediaName", ""), int(info.get("TotalSize", info.get("Size", 0)))))
        return disks

    def open_raw(self, disk: Disk, write: bool = False) -> int:
        """Opens /dev/rdiskN, taking ownership of it first (sudo asks for a
        password once; ownership resets when the disk is replugged)."""
        mode = os.O_RDWR if write else os.O_RDONLY
        if not os.access(disk.raw, os.R_OK | (os.W_OK if write else 0)):
            subprocess.run(["sudo", "chown", os.environ.get("USER", ""), f"/dev/{disk.name}", disk.raw],
                           check=True)
        return os.open(disk.raw, mode)


@dataclass
class Disk:
    name: str          # e.g. "disk4"
    media_name: str    # from the adapter's USB inquiry, often the ATA model
    size: int          # bytes

    @property
    def raw(self) -> str:
        return f"/dev/r{self.name}"
