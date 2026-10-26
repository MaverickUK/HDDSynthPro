"""The macOS side of the rig: finding the drive with diskutil, opening its
raw device, and capturing the system log. Replugging and the rest that
doesn't depend on the OS are in host.py.
"""
from __future__ import annotations

import os
import plistlib
import subprocess

from hddproto.host import Disk, Host


class MacHost(Host):
    def system_log(self, last: str = "3m") -> str:
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

    def _candidates(self) -> list[Disk]:
        out = subprocess.run(["diskutil", "list", "-plist", "external", "physical"],
                             capture_output=True).stdout
        if not out:
            return []
        disks = []
        for name in plistlib.loads(out).get("WholeDisks", []):
            info = plistlib.loads(subprocess.run(["diskutil", "info", "-plist", name],
                                                 capture_output=True).stdout)
            disks.append(Disk(name, info.get("MediaName", ""), int(info.get("TotalSize", info.get("Size", 0))),
                              info.get("IORegistryEntryName", ""), raw=f"/dev/r{name}"))
        return disks

    def open_raw(self, disk: Disk, write: bool = False) -> int:
        """Opens /dev/rdiskN, taking ownership of it first (sudo asks for a
        password once; ownership resets when the disk is replugged)."""
        mode = os.O_RDWR if write else os.O_RDONLY
        if not os.access(disk.raw, os.R_OK | (os.W_OK if write else 0)):
            subprocess.run(["sudo", "chown", os.environ.get("USER", ""), f"/dev/{disk.name}", disk.raw],
                           check=True)
        return os.open(disk.raw, mode)
