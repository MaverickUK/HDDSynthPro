"""The CD-ROM build (bb48_cdrom, docs/atapi.md): the generated test disc
read through the adapter. CD frame n is generated sectors 4n to 4n+3
(hddproto/gen_disk.py), so every byte read can be checked.

It needs a CD-ROM drive serving the test disc (`E cdrom = none` and `!`, or
console `T`): the CD-ROM build, or bb48_hdcd with `device1 = cdrom` (device 1).
Through the Windows rig's adapter it passes in DMA (`mode = mdma2`; 0.30.0-dev.3).

The disc: $HDDPRO_CDROM_DEV if set (e.g. /dev/rdisk5, or \\\\.\\CdRom0 on
Windows, which lists CD-ROM drives apart from disks), else, on macOS, the
external disk whose name says HDDSYNTH or whose size is the test disc's.
"""
from __future__ import annotations

import os
import random
import sys
import time

import pytest

from conftest import require_generated_disk, serves_cdrom
from hddproto import gen_disk, trace
from hddproto.host import make_host, pread

pytestmark = pytest.mark.cdrom

FRAME = 2048
SECTORS_PER_FRAME = FRAME // gen_disk.SECTOR_BYTES
FRAMES = gen_disk.DEFAULT_SECTORS // SECTORS_PER_FRAME  # 32,768: 64 MB
CHUNK_FRAMES = 32  # 64 KB a read


@pytest.fixture(scope="module", autouse=True)
def cdrom_build(drive):
    if not serves_cdrom(drive):
        pytest.skip("needs a CD-ROM drive: PIO_ENV=bb48_cdrom scripts/rig.sh -m cdrom, "
                    "or bb48_hdcd with `device1 = cdrom`")
    require_generated_disk(drive)


@pytest.fixture(scope="module")
def raw_disc(drive):
    """The disc's raw device, read-only: (size in bytes or None, fd)."""
    path = os.environ.get("HDDPRO_CDROM_DEV")
    if path:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
        size = None
    elif sys.platform == "darwin":
        rig = make_host(lambda m: print(f"\n>>> {m}"))
        disk = rig.find_disk(FRAMES * FRAME)
        fd = rig.open_raw(disk)
        size = disk.size
    else:
        pytest.skip("set HDDPRO_CDROM_DEV to the drive's raw device (e.g. \\\\.\\CdRom0)")
    yield size, fd
    os.close(fd)


def read(fd: int, frame: int, count: int) -> bytes:
    data = pread(fd, count * FRAME, frame * FRAME)
    assert len(data) == count * FRAME, f"short read at frame {frame}: {len(data)} bytes"
    return data


def mismatch(data: bytes, frame: int):
    return gen_disk.first_mismatch(data, frame * SECTORS_PER_FRAME)


def test_capacity(raw_disc):
    size, _ = raw_disc
    if size is None:
        pytest.skip("the size comes from diskutil; HDDPRO_CDROM_DEV was given")
    assert size == FRAMES * FRAME


def test_full_disc(raw_disc, drive):
    _, fd = raw_disc
    mark = drive.mark()
    started = time.monotonic()
    for frame in range(0, FRAMES, CHUNK_FRAMES):
        count = min(CHUNK_FRAMES, FRAMES - frame)
        bad = mismatch(read(fd, frame, count), frame)
        assert bad is None, f"mismatch at sector {bad[0]} (frame {bad[0] // SECTORS_PER_FRAME}), byte {bad[1]}"
    seconds = time.monotonic() - started
    print(f"\nRead {FRAMES * FRAME // 2**20} MB in {seconds:.1f} s ({FRAMES * FRAME / seconds / 1e6:.2f} MB/s)")

    # The drive's view: every frame went out through a READ packet without an error.
    drive.wait_quiet(1, timeout=30)
    reads = [c for c in trace.parse(drive.lines_since(mark)).commands
             if c.command == 0xA0 and c.name in ("READ_10", "READ_12")]
    assert all(c.ok for c in reads), [str(c) for c in reads if not c.ok][:5]
    assert sum(c.count for c in reads) >= FRAMES


def test_random_reads(raw_disc):
    _, fd = raw_disc
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    count = int(os.environ.get("HDDPRO_RANDOM_READS", "300"))
    rng = random.Random(seed)
    print(f"\nseed {seed} (rerun with HDDPRO_SEED={seed})")
    for n in range(count):
        length = rng.randint(1, 32)
        frame = rng.randrange(0, FRAMES - length + 1)
        bad = mismatch(read(fd, frame, length), frame)
        assert bad is None, f"read {n} (frame {frame} x {length}): mismatch at sector {bad[0]}, byte {bad[1]}"


def test_first_and_last_frames(raw_disc):
    _, fd = raw_disc
    for frame in (0, 1, FRAMES - 2, FRAMES - 1):
        assert mismatch(read(fd, frame, 1), frame) is None
