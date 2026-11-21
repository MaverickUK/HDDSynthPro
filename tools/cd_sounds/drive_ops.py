"""What a CD-ROM drive does to make each of its sounds, over any transport
with a `command(cdb, data_in=0, timeout_s=30)` method (scsi_win.CdDrive, or a
fake in the tests)."""
from __future__ import annotations

import random
import time
from dataclasses import dataclass, field

import scsi


@dataclass
class DiscInfo:
    sectors: int = 0
    name: str = ""
    max_x: float = 0.0
    tray: str = "?"
    speed_ok: bool | None = None  # SET CD SPEED accepted (None: not tried)
    notes: list[str] = field(default_factory=list)


class Ops:
    def __init__(self, drive, log=print):
        self.drive = drive
        self.log = log
        self.sectors = 0

    # ---- state ----------------------------------------------------------------

    def ready(self) -> bool:
        return self.drive.command(scsi.test_unit_ready(), timeout_s=10).ok

    def wait_ready(self, timeout_s: float = 30.0) -> float:
        """Polls TEST UNIT READY until the disc is ready (spun up and read);
        returns how long it took, or -1 if it never was."""
        start = time.perf_counter()
        while time.perf_counter() - start < timeout_s:
            r = self.drive.command(scsi.test_unit_ready(), timeout_s=10)
            if r.ok:
                return time.perf_counter() - start
            time.sleep(0.05)
        return -1.0

    def info(self) -> DiscInfo:
        d = DiscInfo()
        r = self.drive.command(scsi.inquiry(), data_in=96)
        if r.ok:
            d.name = scsi.parse_inquiry(r.data).name
        r = self.drive.command(scsi.mode_sense10(0x2A, 64), data_in=64)
        caps = scsi.parse_capabilities(r.data) if r.ok else None
        if caps:
            d.max_x, d.tray = caps.max_x, caps.tray
        else:
            d.notes.append(f"no capabilities page (MODE SENSE 2Ah: {r})")
        self.wait_ready(20)
        r = self.drive.command(scsi.read_capacity(), data_in=8)
        if r.ok:
            d.sectors, _ = scsi.parse_capacity(r.data)
            self.sectors = d.sectors
        else:
            d.notes.append(f"no capacity (READ CAPACITY: {r}): is a data disc in?")
        return d

    # ---- the motor and the tray ------------------------------------------------

    def spin_up(self):
        return self.drive.command(scsi.start_stop(True), timeout_s=30)

    def spin_down(self):
        return self.drive.command(scsi.start_stop(False), timeout_s=30)

    def open_tray(self):
        return self.drive.command(scsi.start_stop(False, load_eject=True), timeout_s=30)

    def close_tray(self):
        return self.drive.command(scsi.start_stop(True, load_eject=True), timeout_s=30)

    def set_speed(self, x: float | None):
        """`None`: the fastest. Returns the command's result."""
        kbs = 0xFFFF if x is None else int(round(x * scsi.KX))
        return self.drive.command(scsi.set_cd_speed(kbs))

    # ---- the laser ---------------------------------------------------------------

    def read_at(self, lba: int, count: int = 1):
        lba = max(0, min(lba, max(0, self.sectors - count)))
        return self.drive.command(scsi.read10(lba, count), data_in=count * scsi.SECTOR)

    def seek_radius(self, r_mm: float):
        """A one-sector read where the laser is at `r_mm` (25-58 mm): a seek
        there, which every drive does (SEEK alone is optional for some)."""
        return self.read_at(min(scsi.lba_at_radius(r_mm), self.sectors - 1))

    def read_for(self, seconds: float, start_lba: int, chunk: int = 32) -> int:
        """Reads sequentially from `start_lba` for `seconds`: the drive's
        sound while it streams. Returns the sectors read."""
        lba, done = start_lba, 0
        end = time.perf_counter() + seconds
        while time.perf_counter() < end:
            if lba + chunk >= self.sectors:
                lba = start_lba
            if not self.read_at(lba, chunk).ok:
                break
            lba += chunk
            done += chunk
        return done

    def read_rate_x(self, seconds: float = 2.0) -> float:
        """How fast the drive reads now, in x, a third of the way out (after
        a moment to reach its speed)."""
        lba = self.sectors // 3
        self.read_at(lba, 1)
        time.sleep(1.5)
        start = time.perf_counter()
        n = self.read_for(seconds, lba)
        return n * scsi.SECTOR / (time.perf_counter() - start) / 1024 / scsi.KX

    def speed_obeyed(self) -> bool:
        """Whether the drive really reads slower when asked: some accept SET CD
        SPEED and go on at their own speed (the LG GCC-4320B, 21 Nov 2026).
        Leaves it at the fastest."""
        self.set_speed(None)
        fastest = self.read_rate_x()
        self.set_speed(4)
        slow = self.read_rate_x()
        self.set_speed(None)
        return slow < fastest * 0.7

    def random_seeks(self, count: int, gap_s: float, rng: random.Random, lo_mm: float = 25.0,
                     hi_mm: float | None = None) -> list[float]:
        """Seeks to random radii, up to the disc's last data (a part-filled
        disc's far seeks would otherwise all read one sector, from the cache)."""
        hi_mm = min(hi_mm or 58.0, self.max_radius())
        radii = []
        for _ in range(count):
            r = rng.uniform(lo_mm, hi_mm)
            self.seek_radius(r)
            radii.append(r)
            if gap_s:
                time.sleep(gap_s)
        return radii

    def max_radius(self) -> float:
        """How far out this disc's data goes."""
        return scsi.radius_mm(self.sectors - 1) if self.sectors else 25.0
