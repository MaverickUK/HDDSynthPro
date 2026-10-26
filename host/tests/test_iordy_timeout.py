"""How long does the adapter really wait on IORDY? Core 1 answers every data
read later by a debug delay (console `D` and a digit: 250 ns steps), and at
each step random reads are checked byte for byte. A host that gives up on
IORDY samples DD before the drive has driven it, so reads go wrong from some
delay on. The IORDY timer shows how long reads were actually held.

Exploratory: it prints a table, and only fails if reads go wrong with no
delay at all."""
from __future__ import annotations

import os
import random
import re

import pytest

from conftest import require_build, require_generated_disk
from hddproto import gen_disk
from hddproto.host import pread

pytestmark = pytest.mark.timing

SECTOR = gen_disk.SECTOR_BYTES
READ_BYTES = 2 << 20  # per step


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


@pytest.fixture(scope="module", autouse=True)
def generated_disk(drive):
    require_generated_disk(drive)


def stats(drive) -> dict:
    return dict(re.findall(r"(\w+)=(\S+)", drive.command("s", "STATS")))


def bits_lost(got: bytes, want: bytes) -> tuple[int, int]:
    """Bits that read 0 but should be 1, and the other way round."""
    lost = gained = 0
    for a, b in zip(got, want):
        lost += bin(b & ~a & 0xFF).count("1")
        gained += bin(a & ~b & 0xFF).count("1")
    return lost, gained


def test_iordy_timeout(raw_disk, drive, run_dir):
    _, fd = raw_disk
    rng = random.Random(1)
    rows = []
    try:
        for digit in range(10):
            delay_ns = digit * 250
            assert drive.command(f"D{digit}", "DELAY").split()[1] == str(delay_ns)
            drive.send("r")
            bad_sectors = lost = gained = 0
            moved = 0
            while moved < READ_BYTES:
                count = rng.randint(1, 256)
                lba = rng.randrange(0, gen_disk.DEFAULT_SECTORS - count + 1)
                try:
                    data = pread(fd, count * SECTOR, lba * SECTOR)
                except OSError as e:
                    rows.append((delay_ns, "-", f"I/O error: {e}", "", ""))
                    break
                moved += len(data)
                for k in range(count):
                    got = data[k * SECTOR:(k + 1) * SECTOR]
                    want = gen_disk.sector(lba + k)
                    if got != want:
                        bad_sectors += 1
                        l, g = bits_lost(got, want)
                        lost, gained = lost + l, gained + g
            else:
                f = stats(drive)
                rows.append((delay_ns, f["iordy_max_ns"], bad_sectors, lost, gained))
                continue
            break  # an I/O error: the adapter has had enough
    finally:
        drive.command("D0", "DELAY")
    lines = ["delay_ns  iordy_max_ns  bad_sectors  bits_1to0  bits_0to1"]
    lines += [f"{d:8}  {m:>12}  {b:>11}  {l:>9}  {g:>9}" for d, m, b, l, g in rows]
    (run_dir / "iordy_timeout.txt").write_text("\n".join(lines) + "\n")
    print("\n" + "\n".join(lines))
    assert rows and rows[0][2] == 0, "reads went wrong with no delay"
