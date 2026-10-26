"""The Windows side of the rig: finding the drive with Get-Disk, opening its
raw device, and capturing the system event log. Replugging and the rest that
doesn't depend on the OS are in host.py. (uhubctl hardly works on Windows,
so expect to replug by hand.)

Raw access to \\\\.\\PhysicalDriveN needs an administrator terminal. It goes
straight to the disk driver, past the file cache, so every byte read came
from the drive. Windows refuses raw writes inside a mounted volume; the
generated disk has no file system, so none is mounted. If Windows offers to
format it, cancel.
"""
from __future__ import annotations

import json
import os
import subprocess

from hddproto.host import Disk, Host


def powershell(script: str) -> subprocess.CompletedProcess:
    return subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                          capture_output=True, text=True)


class WindowsHost(Host):
    def system_log(self, last: str = "3m") -> str:
        minutes = int(last.removesuffix("m"))
        result = powershell(
            "Get-WinEvent -FilterHashtable @{LogName='System'; "
            f"StartTime=(Get-Date).AddMinutes(-{minutes})}} -ErrorAction SilentlyContinue | "
            "Where-Object ProviderName -match 'disk|usb|stor|partmgr|Kernel-PnP' | "
            "Format-Table -Wrap TimeCreated, ProviderName, Id, Message | Out-String -Width 250")
        return result.stdout + result.stderr

    def disks(self) -> str:
        return powershell("Get-Disk | Where-Object BusType -eq 'USB' | "
                          "Format-Table Number, FriendlyName, Size, PartitionStyle | Out-String").stdout

    def _candidates(self) -> list[Disk]:
        result = powershell(
            "ConvertTo-Json -Compress @(Get-Disk | Where-Object BusType -eq 'USB' | "
            "Select-Object Number, FriendlyName, Model, Size)")
        if result.returncode or not result.stdout.strip():
            return []
        return [Disk(f"PhysicalDrive{d['Number']}", (d["Model"] or "").strip(), int(d["Size"]),
                     (d["FriendlyName"] or "").strip(), raw=f"\\\\.\\PhysicalDrive{d['Number']}")
                for d in json.loads(result.stdout)]

    def open_raw(self, disk: Disk, write: bool = False) -> int:
        mode = (os.O_RDWR if write else os.O_RDONLY) | os.O_BINARY
        try:
            return os.open(disk.raw, mode)
        except PermissionError as e:
            raise PermissionError(f"can't open {disk.raw}: run the tests from an administrator "
                                  "terminal") from e
