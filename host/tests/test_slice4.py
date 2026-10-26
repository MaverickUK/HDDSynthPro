"""Slice 4: write to a RAM disk. Writes land in a RAM overlay on the generated
disk (src/block_source.h), so every written sector reads back and the rest
of the disk keeps its generated content.

All writes go to one window of WINDOW sectors, well inside the overlay's 512,
so the test can be run again without filling it. The drive keeps the overlay
until it reboots, so each test writes the generated content back afterwards:
slice 3 still finds the disk it expects.

Exit criterion: 1,000 write-then-read cycles with no mismatches.
"""
from __future__ import annotations

import os
import random
import time

import pytest

from conftest import require_build, require_generated_disk
from hddproto import gen_disk, trace
from hddproto.host import pread, pwrite

pytestmark = pytest.mark.slice4


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


@pytest.fixture(scope="module", autouse=True)
def generated_disk(drive):
    require_generated_disk(drive)  # before the raw disk is opened


SECTOR = gen_disk.SECTOR_BYTES
WINDOW_START = 100_000
WINDOW = 256


@pytest.fixture(autouse=True)
def restore_window(raw_disk_rw):
    yield
    _, fd = raw_disk_rw
    write(fd, WINDOW_START, b"".join(gen_disk.sector(WINDOW_START + i) for i in range(WINDOW)))


def read(fd: int, lba: int, count: int) -> bytes:
    data = pread(fd, count * SECTOR, lba * SECTOR)
    assert len(data) == count * SECTOR, f"short read at LBA {lba}: {len(data)} bytes"
    return data


def write(fd: int, lba: int, data: bytes):
    assert pwrite(fd, data, lba * SECTOR) == len(data), f"short write at LBA {lba}"


def first_diff(got: bytes, want: bytes, lba: int) -> str | None:
    for i, (a, b) in enumerate(zip(got, want)):
        if a != b:
            return f"LBA {lba + i // SECTOR}, byte {i % SECTOR}: got {a:#04x}, want {b:#04x}"
    return None


def test_write_then_read_cycles(raw_disk_rw, drive):
    _, fd = raw_disk_rw
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    cycles = int(os.environ.get("HDDPRO_WRITE_CYCLES", "1000"))
    rng = random.Random(seed)
    print(f"\nseed {seed} (rerun with HDDPRO_SEED={seed})")

    # The window starts as whatever is there (earlier runs may have written it).
    shadow = bytearray(read(fd, WINDOW_START, WINDOW))
    mark = drive.mark()
    started = time.monotonic()
    written = 0
    for n in range(cycles):
        count = rng.randint(1, 32)
        offset = rng.randrange(0, WINDOW - count + 1)
        data = rng.randbytes(count * SECTOR)
        write(fd, WINDOW_START + offset, data)
        shadow[offset * SECTOR:(offset + count) * SECTOR] = data
        written += count
        bad = first_diff(read(fd, WINDOW_START + offset, count), data, WINDOW_START + offset)
        assert bad is None, f"cycle {n} (LBA {WINDOW_START + offset} x {count}): {bad}"
    seconds = time.monotonic() - started
    print(f"{cycles} cycles, {written} sectors written and read back in {seconds:.1f} s "
          f"({2 * written * SECTOR / seconds / 1e6:.2f} MB/s both ways)")

    # A write that landed in the wrong place shows up here.
    bad = first_diff(read(fd, WINDOW_START, WINDOW), bytes(shadow), WINDOW_START)
    assert bad is None, f"window doesn't match what was written: {bad}"

    # The drive's view: every WRITE SECTORS completed without an error.
    drive.wait_quiet(1, timeout=30)
    t = trace.parse(drive.lines_since(mark))
    writes = [c for c in t.commands if c.command in (0x30, 0x31)]
    assert writes, "no WRITE SECTORS in the drive's trace"
    assert all(c.ok for c in writes), [str(c) for c in writes if not c.ok][:5]


def test_long_write(raw_disk_rw):
    # The whole window in one call: macOS sends it as WRITE SECTORS commands of
    # up to 128 sectors. (A count of 0, meaning 256, is unit-tested in C++.)
    _, fd = raw_disk_rw
    data = random.Random(4).randbytes(WINDOW * SECTOR)
    write(fd, WINDOW_START, data)
    bad = first_diff(read(fd, WINDOW_START, WINDOW), data, WINDOW_START)
    assert bad is None, bad


def test_outside_the_window_untouched(raw_disk_rw):
    _, fd = raw_disk_rw
    for lba in (WINDOW_START - 1, WINDOW_START + WINDOW):
        assert gen_disk.first_mismatch(read(fd, lba, 1), lba) is None, f"LBA {lba} changed"
