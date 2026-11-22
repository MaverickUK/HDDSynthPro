"""SCSI commands to a CD-ROM drive on macOS, through IOKit's MMC device
interface (the one disc burning programs use), by a small helper in C
(scsi_mac.c, built with clang on first use: Xcode's command line tools).

macOS gives the helper the drive only while its disc isn't mounted, so
CdDrive unmounts it first (`diskutil unmountDisk`); macOS mounts it again
when the helper lets go. No administrator rights needed.
"""
from __future__ import annotations

import struct
import subprocess
import time
from pathlib import Path

from scsi import Result, parse_sense

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "scsi_mac.c"
BINARY = HERE / "runs" / "scsi_mac"

DIR_NONE, DIR_IN, DIR_OUT = 0, 1, 2
REPLY = struct.Struct("<iB32sII")  # IOReturn, SCSI status, sense, bytes moved, microseconds


def helper() -> Path:
    """The helper, built if it's missing or older than its source."""
    if not BINARY.exists() or BINARY.stat().st_mtime < SOURCE.stat().st_mtime:
        BINARY.parent.mkdir(exist_ok=True)
        cmd = ["clang", "-O2", "-Wall", "-o", str(BINARY), str(SOURCE), "-framework", "IOKit",
               "-framework", "CoreFoundation"]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise OSError(f"couldn't build {SOURCE.name} (xcode-select --install?):\n{p.stderr}")
    return BINARY


def list_cd_drives() -> list[tuple[str, str, str]]:
    """(path, BSD name, name) for each CD/DVD drive; the path is its index."""
    out = subprocess.run([str(helper()), "list"], capture_output=True, text=True).stdout
    drives = []
    for line in out.splitlines():
        parts = line.split("|", 2)
        if len(parts) == 3:
            drives.append((parts[0], parts[1] or "(no disc)", parts[2].strip()))
    return drives


def _index(path: str) -> tuple[str, str]:
    """A drive's index and BSD name, from either (`0`, `disk4`, `/dev/disk4`)."""
    want = path.removeprefix("/dev/").removeprefix("r")
    for index, bsd, _name in list_cd_drives():
        if want in (index, bsd):
            return index, bsd
    raise OSError(f"no CD drive {path!r} (python probe.py lists them)")


def _encode(cdb: bytes, direction: int, length: int, timeout_ms: int) -> bytes:
    return bytes([len(cdb)]) + cdb.ljust(16, b"\0")[:16] + struct.pack("<BII", direction, length, timeout_ms)


class CdDrive:
    """One CD-ROM drive, by its index in `list_cd_drives()` or its BSD name
    (`disk4`). `argv` replaces the helper (for tests)."""

    def __init__(self, path: str, argv: list[str] | None = None):
        if argv is None:
            index, bsd = _index(path)
            if bsd.startswith("disk"):
                # the helper can only have the drive while nothing on the disc is mounted
                subprocess.run(["diskutil", "unmountDisk", f"/dev/{bsd}"], capture_output=True)
            argv = [str(helper()), "open", index]
        self.path = path
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        line = self.proc.stdout.readline().decode(errors="replace").strip()
        if line != "READY":
            self.close()
            raise OSError(f"can't open CD drive {path}: {line or 'the helper exited'}")

    def close(self):
        if self.proc:
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.kill()
            self.proc = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read(self, n: int) -> bytes:
        data = self.proc.stdout.read(n)
        if len(data) != n:
            raise OSError("the CD drive helper stopped")
        return data

    def command(self, cdb: bytes, data_in: int = 0, data_out: bytes | None = None, timeout_s: int = 30) -> Result:
        """Sends `cdb`; reads `data_in` bytes back, or writes `data_out`."""
        if data_out is not None:
            req = _encode(cdb, DIR_OUT, len(data_out), timeout_s * 1000) + data_out
        elif data_in:
            req = _encode(cdb, DIR_IN, data_in, timeout_s * 1000)
        else:
            req = _encode(cdb, DIR_NONE, 0, timeout_s * 1000)
        start = time.perf_counter()
        self.proc.stdin.write(req)
        self.proc.stdin.flush()
        err, status, sense, moved, _us = REPLY.unpack(self._read(REPLY.size))
        data = self._read(moved) if moved else b""
        seconds = time.perf_counter() - start
        if err != 0:
            return Result(False, -1, b"", None, seconds, f"IOKit error 0x{err & 0xFFFFFFFF:08x}")
        return Result(status == 0, status, data, parse_sense(sense) if status else None, seconds)
