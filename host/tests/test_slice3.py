"""Slice 3: read from a generated disk. Every sector's content is computed
from its LBA (hddproto/gen_disk.py, twin of src/gen_disk.h), so whatever
the host reads can be checked byte for byte.

Reads go through the raw device (/dev/rdiskN), which macOS does not cache,
so every byte checked really came from the drive. The disk has no file
system: when macOS says it is not readable, choose Ignore.

Exit criterion: the full-disk and random-read tests pass with zero
mismatches.

Not covered here: a read past the end of the disk. macOS knows the size from
IDENTIFY and refuses such reads itself, so they never reach the drive; the
drive's own range check is unit-tested in C++ instead.
"""
from __future__ import annotations

import os
import random
import time

import pytest

from conftest import require_build, require_generated_disk
from hddproto import gen_disk, trace
from hddproto.host import pread

pytestmark = pytest.mark.slice3


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


@pytest.fixture(scope="module", autouse=True)
def generated_disk(drive):
    require_generated_disk(drive)  # before the raw disk is opened


SECTOR = gen_disk.SECTOR_BYTES
TOTAL = gen_disk.DEFAULT_SECTORS
CHUNK_SECTORS = 256  # one READ SECTORS command's worth


def read(fd: int, lba: int, count: int) -> bytes:
    data = pread(fd, count * SECTOR, lba * SECTOR)
    assert len(data) == count * SECTOR, f"short read at LBA {lba}: {len(data)} bytes"
    return data


def test_capacity(raw_disk):
    disk, _ = raw_disk
    assert disk.size == TOTAL * SECTOR


def test_full_disk(raw_disk, drive):
    _, fd = raw_disk
    mark = drive.mark()
    started = time.monotonic()
    for lba in range(0, TOTAL, CHUNK_SECTORS):
        count = min(CHUNK_SECTORS, TOTAL - lba)
        bad = gen_disk.first_mismatch(read(fd, lba, count), lba)
        assert bad is None, f"mismatch at LBA {bad[0]}, byte {bad[1]}"
    seconds = time.monotonic() - started
    print(f"\nRead {TOTAL * SECTOR // 2**20} MB in {seconds:.1f} s "
          f"({TOTAL * SECTOR / seconds / 1e6:.2f} MB/s)")

    # The drive's view: every sector went out through READ SECTORS without an error.
    drive.wait_quiet(1, timeout=30)
    reads = [c for c in trace.parse(drive.lines_since(mark)).commands if c.command in (0x20, 0x21)]
    assert all(c.ok for c in reads), [str(c) for c in reads if not c.ok][:5]
    assert sum(c.count for c in reads) >= TOTAL


def test_random_reads(raw_disk):
    _, fd = raw_disk
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    count = int(os.environ.get("HDDPRO_RANDOM_READS", "1000"))
    rng = random.Random(seed)
    print(f"\nseed {seed} (rerun with HDDPRO_SEED={seed})")
    for n in range(count):
        length = rng.randint(1, 256)
        lba = rng.randrange(0, TOTAL - length + 1)
        bad = gen_disk.first_mismatch(read(fd, lba, length), lba)
        assert bad is None, f"read {n} (LBA {lba} x {length}): mismatch at LBA {bad[0]}, byte {bad[1]}"


def test_first_and_last_sectors(raw_disk):
    _, fd = raw_disk
    for lba in (0, 1, TOTAL - 2, TOTAL - 1):
        assert read(fd, lba, 1) == gen_disk.sector(lba)
    try:
        past_end = pread(fd, SECTOR, TOTAL * SECTOR)  # macOS stops at the end
    except OSError:
        past_end = b""  # Windows refuses the read (EINVAL)
    assert past_end == b""
