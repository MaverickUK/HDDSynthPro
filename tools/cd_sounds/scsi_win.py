"""SCSI commands to a CD-ROM drive on Windows, through the SCSI pass-through
(IOCTL_SCSI_PASS_THROUGH_DIRECT): the command block goes to the drive as it
is, through the USB-IDE adapter, which wraps it for the drive's ATAPI.

Needs an administrator terminal. Standard library only (ctypes).
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import re
import subprocess
import time

from scsi import Result, parse_sense

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x4D014
DATA_OUT, DATA_IN, DATA_NONE = 0, 1, 2
INVALID_HANDLE = ctypes.c_void_p(-1).value


class _SPTD(ctypes.Structure):
    _fields_ = [("Length", wt.USHORT), ("ScsiStatus", ctypes.c_ubyte), ("PathId", ctypes.c_ubyte),
                ("TargetId", ctypes.c_ubyte), ("Lun", ctypes.c_ubyte), ("CdbLength", ctypes.c_ubyte),
                ("SenseInfoLength", ctypes.c_ubyte), ("DataIn", ctypes.c_ubyte),
                ("DataTransferLength", wt.ULONG), ("TimeOutValue", wt.ULONG), ("DataBuffer", ctypes.c_void_p),
                ("SenseInfoOffset", wt.ULONG), ("Cdb", ctypes.c_ubyte * 16)]


class _SPTDWithSense(ctypes.Structure):
    _fields_ = [("sptd", _SPTD), ("filler", wt.ULONG), ("sense", ctypes.c_ubyte * 32)]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.restype = wt.HANDLE
_k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
_k32.DeviceIoControl.argtypes = [wt.HANDLE, wt.DWORD, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, wt.DWORD,
                                 ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
_k32.CloseHandle.argtypes = [wt.HANDLE]


class CdDrive:
    """One CD-ROM drive, by `\\\\.\\CdRomN` or a drive letter (`E:`)."""

    def __init__(self, path: str):
        if re.fullmatch(r"[A-Za-z]:?", path):
            path = f"\\\\.\\{path[0]}:"
        elif not path.startswith("\\\\.\\"):
            path = "\\\\.\\" + path
        self.path = path
        self.handle = _k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       None, OPEN_EXISTING, 0, None)
        if self.handle in (None, INVALID_HANDLE):
            err = ctypes.get_last_error()
            hint = " (run from an administrator terminal)" if err == 5 else ""
            raise OSError(err, f"can't open {path}: {ctypes.FormatError(err)}{hint}")

    def close(self):
        if self.handle not in (None, INVALID_HANDLE):
            _k32.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def command(self, cdb: bytes, data_in: int = 0, data_out: bytes | None = None, timeout_s: int = 30) -> Result:
        """Sends `cdb`; reads `data_in` bytes back, or writes `data_out`."""
        req = _SPTDWithSense()
        s = req.sptd
        s.Length = ctypes.sizeof(_SPTD)
        s.CdbLength = len(cdb)
        s.SenseInfoLength = len(req.sense)
        s.SenseInfoOffset = _SPTDWithSense.sense.offset
        s.TimeOutValue = timeout_s
        buf = None
        if data_out is not None:
            buf = ctypes.create_string_buffer(data_out, len(data_out))
            s.DataIn, s.DataTransferLength = DATA_OUT, len(data_out)
        elif data_in:
            buf = ctypes.create_string_buffer(data_in)
            s.DataIn, s.DataTransferLength = DATA_IN, data_in
        else:
            s.DataIn = DATA_NONE
        s.DataBuffer = ctypes.cast(buf, ctypes.c_void_p).value if buf is not None else None
        for i, b in enumerate(cdb):
            s.Cdb[i] = b
        returned = wt.DWORD(0)
        start = time.perf_counter()
        ok = _k32.DeviceIoControl(self.handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, ctypes.byref(req), ctypes.sizeof(req),
                                  ctypes.byref(req), ctypes.sizeof(req), ctypes.byref(returned), None)
        seconds = time.perf_counter() - start
        if not ok:
            err = ctypes.get_last_error()
            return Result(False, -1, b"", None, seconds, f"{ctypes.FormatError(err)} ({err})")
        data = bytes(buf.raw[:s.DataTransferLength]) if (buf is not None and data_out is None) else b""
        sense = parse_sense(bytes(req.sense)) if s.ScsiStatus else None
        return Result(s.ScsiStatus == 0, s.ScsiStatus, data, sense, seconds)


def list_cd_drives() -> list[tuple[str, str, str]]:
    """(path, drive letter, name) for each CD-ROM drive Windows has."""
    ps = ("Get-CimInstance Win32_CDROMDrive | ForEach-Object { \"$($_.Drive)|$($_.Caption)|$($_.DeviceID)\" }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    drives = []
    for line in out.splitlines():
        parts = line.strip().split("|")
        if len(parts) == 3 and parts[0]:
            drives.append((f"\\\\.\\{parts[0]}", parts[0], parts[1]))
    return drives
