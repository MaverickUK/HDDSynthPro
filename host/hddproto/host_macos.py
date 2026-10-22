"""The macOS side of the rig: replugging the USB-IDE adapter and capturing
the system log. Everything specific to the host OS lives here, so a Linux
version (needed for ATA pass-through in slice 6) can sit alongside it.

Replug, in order of preference:
  1. HDDPRO_UHUBCTL set: uhubctl power-cycles the adapter's hub port.
     Its value is the uhubctl location and port, e.g. "-l 20-2 -p 3".
  2. Otherwise the test asks you to do it by hand and waits.
"""
from __future__ import annotations

import os
import shlex
import subprocess


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
            subprocess.run(["uhubctl", "-a", "cycle", "-d", "2", *shlex.split(self._uhubctl)],
                           check=True, capture_output=True)
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
