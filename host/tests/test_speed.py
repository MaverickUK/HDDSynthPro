"""Throughput, for the speed log (docs/speed-log.md). Measured from the Mac
through the raw device, and from the drive's own CMD/DONE lines:
  - sequential reads, 64 KB requests (the adapter's largest command, 128
    sectors);
  - random 4 KB reads;
  - sequential writes, 64 KB requests, into the RAM overlay window that
    slice 4 and the soak use (generated disk only: never on an SD image);
  - where a read's time goes: commands of 1 to 128 sectors, timed by the
    drive, fitted to a fixed cost per command plus a cost per sector.

It prints a table and saves it as speed.txt in the run's folder. It only
fails if the drive reports a failed command."""
from __future__ import annotations

import os
import random
import re
import time

import pytest

from conftest import require_build
from hddproto import gen_disk, trace
from hddproto.host import make_host, pread, pwrite

pytestmark = pytest.mark.speed

SECTOR = gen_disk.SECTOR_BYTES
MB = 1_000_000
SEQ_READ_BYTES = 32 << 20
SEQ_WRITE_BYTES = 16 << 20
REQUEST = 64 << 10
RANDOM_READS = 500
WINDOW_START = 100_000  # as in test_slice4.py and test_soak.py
WINDOW = 256
FIT_SIZES = [1, 8, 32, 64, 128]
FIT_REPEATS = 20


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def stats(drive) -> dict:
    return dict(re.findall(r"(\w+)=(\S+)", drive.command("s", "STATS")))


def commands_since(drive, mark: int, name: str) -> list[trace.Command]:
    time.sleep(0.3)  # let the last DONE lines arrive
    t = trace.parse(drive.lines_since(mark))
    failed = [str(c) for c in t.commands if c.status is not None and not c.ok]
    assert not failed, f"failed commands: {failed[:5]}"
    return [c for c in t.commands if c.name == name and c.duration_us is not None]


def fit(points: list[tuple[int, float]]) -> tuple[float, float]:
    """Least squares: duration = fixed + per_sector * count."""
    n = len(points)
    sx = sum(x for x, _ in points)
    sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points)
    sxy = sum(x * y for x, y in points)
    per = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    return (sy - per * sx) / n, per


def test_speed(raw_disk, drive, run_dir):
    _, fd = raw_disk
    rng = random.Random(1)
    f = stats(drive)
    disk = f.get("disk", "?")
    sectors = int(f.get("sectors", gen_disk.DEFAULT_SECTORS))
    lines = [f"firmware {drive.command('h', 'READY').split()[2]}, disk {disk}"]
    drive.send("r")  # read timings from here: traffic only, not the detection after a replug

    # Sequential reads
    mark = drive.mark()
    start = time.monotonic()
    offset = 0
    while offset < SEQ_READ_BYTES:
        assert len(pread(fd, REQUEST, offset)) == REQUEST
        offset += REQUEST
    seconds = time.monotonic() - start
    reads = commands_since(drive, mark, "READ_SECTORS")
    per_cmd = sum(c.duration_us for c in reads) / len(reads) / 1000
    lines.append(f"sequential read   {SEQ_READ_BYTES / MB / seconds:6.2f} MB/s   "
                 f"({len(reads)} commands of {reads[0].count} sectors, {per_cmd:.1f} ms each in the drive)")

    # Random 4 KB reads
    start = time.monotonic()
    for _ in range(RANDOM_READS):
        lba = rng.randrange(0, sectors - 8) & ~7
        assert len(pread(fd, 4096, lba * SECTOR)) == 4096
    seconds = time.monotonic() - start
    lines.append(f"random 4 KB read  {RANDOM_READS / seconds:6.1f} IOPS   ({seconds / RANDOM_READS * 1000:.1f} ms each)")

    # Where a read's time goes
    mark = drive.mark()
    for count in FIT_SIZES:
        for _ in range(FIT_REPEATS):
            lba = rng.randrange(0, sectors - count)
            pread(fd, count * SECTOR, lba * SECTOR)
    points = [(c.count, c.duration_us / 1000) for c in commands_since(drive, mark, "READ_SECTORS")]
    fixed, per_sector = fit(points)
    lines.append(f"read command      {fixed:6.2f} ms fixed + {per_sector * 1000:.0f} us per sector "
                 f"(the bus and fetches: {SECTOR / per_sector / 1000:.2f} MB/s)")

    # Sequential writes: on the generated disk (its RAM overlay) or the scratch
    # image HDDBENCH.IMG, never on HDDSYNTH.IMG
    if disk in ("generated", "bench"):
        wfd = make_host(print).open_raw(raw_disk[0], write=True)
        try:
            mark = drive.mark()
            data = rng.randbytes(REQUEST)
            start = time.monotonic()
            written = 0
            while written < SEQ_WRITE_BYTES:
                half = (written // REQUEST) % 2  # the window's two 64 KB halves in turn
                base = WINDOW_START + half * (REQUEST // SECTOR)
                assert pwrite(wfd, data, base * SECTOR) == REQUEST
                written += REQUEST
            seconds = time.monotonic() - start
            writes = commands_since(drive, mark, "WRITE_SECTORS")
            per_cmd = sum(c.duration_us for c in writes) / len(writes) / 1000
            lines.append(f"sequential write  {SEQ_WRITE_BYTES / MB / seconds:6.2f} MB/s   "
                         f"({len(writes)} commands of {writes[0].count} sectors, {per_cmd:.1f} ms each in the drive)")
        finally:
            pwrite(wfd, gen_disk.sectors(WINDOW_START, WINDOW), WINDOW_START * SECTOR)
            os.close(wfd)
    else:
        lines.append("sequential write  (skipped: not on HDDSYNTH.IMG; use the bench image)")

    f = stats(drive)
    lines.append(f"worst read {f.get('iordy_max_ns')} ns ({f.get('iordy_max_reg')}), early {f.get('iordy_early')}")
    (run_dir / "speed.txt").write_text("\n".join(lines) + "\n")
    print("\n" + "\n".join(lines))
