"""Can the drive tell sequential from random activity? The HDD Synth Pro
needs this to choose between a sequential and a random sound, where the
original HDD Synth only had the host's activity LED.

The Mac runs phases of known activity against the drive through the raw
device; the drive's classifier (src/activity.h) reports its state in ACT
lines. Each phase is bracketed by MARK lines (console `m`), so the trace's
own clock says where each phase starts and ends.

Exit criterion: each phase spends at least 90% of its time in the right
state, and the drive goes IDLE when the host stops.

Needs the drive build (it has the generated disk to read).
"""
from __future__ import annotations

import os
import random
import time

import pytest

from conftest import require_build
from hddproto import gen_disk, trace

pytestmark = pytest.mark.activity

SECTOR = gen_disk.SECTOR_BYTES
TOTAL = gen_disk.DEFAULT_SECTORS
MIN_SHARE = 0.90


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def mark(drive) -> int:
    return int(drive.command("m", "MARK").split()[1])


def read(fd, lba, sectors):
    data = os.pread(fd, sectors * SECTOR, lba * SECTOR)
    assert len(data) == sectors * SECTOR


def sequential(fd, start_lba=1000, megabytes=8, chunk_sectors=128):
    for lba in range(start_lba, start_lba + megabytes * 2048, chunk_sectors):
        read(fd, lba, chunk_sectors)


def scattered(fd, rng, reads=300, sectors=8):
    for _ in range(reads):
        read(fd, rng.randrange(0, TOTAL - sectors), sectors)


def interleaved(fd, megabytes=4, chunk_sectors=64):
    a, b = 2000, 80000
    for _ in range(megabytes * 2048 // (2 * chunk_sectors)):
        read(fd, a, chunk_sectors)
        a += chunk_sectors
        read(fd, b, chunk_sectors)
        b += chunk_sectors


def test_tells_sequential_from_random(drive, raw_disk, run_dir):
    _, fd = raw_disk
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    rng = random.Random(seed)
    first = drive.mark()

    phases = []
    for name, want, work in [
        ("sequential", "SEQ", lambda: sequential(fd)),
        ("idle", "IDLE", lambda: time.sleep(2.0)),  # the first ~150 ms reads as the last state
        ("random", "RAND", lambda: scattered(fd, rng)),
        ("idle", "IDLE", lambda: time.sleep(2.0)),  # the first ~150 ms reads as the last state
        ("interleaved", "SEQ", lambda: interleaved(fd)),
    ]:
        start = mark(drive)
        work()
        end = mark(drive)
        phases.append((name, want, start, end))
    drive.wait_quiet(1, timeout=30)

    lines = drive.lines_since(first)
    (run_dir / "trace.txt").write_text("\n".join(lines) + "\n")
    t = trace.parse(lines)

    report = [f"seed {seed} (rerun with HDDPRO_SEED={seed})"]
    failures = []
    for name, want, start, end in phases:
        spent = trace.time_in_states(t.activity, start, end)
        total = max(1, end - start)
        share = spent[want] / total
        commands = [c for c in t.commands if start <= c.t_us < end and c.command in (0x20, 0x21)]
        sizes = sorted({c.count for c in commands})
        report.append(f"{name:12} {total / 1e6:5.2f} s  want {want:4} {share:6.1%}  "
                      + "  ".join(f"{k}={v / total:.0%}" for k, v in spent.items())
                      + f"  ({len(commands)} reads, sizes {sizes[:6]})")
        if share < MIN_SHARE:
            failures.append(f"{name}: {share:.1%} {want}")
    print("\n" + "\n".join(report))
    (run_dir / "summary.txt").write_text("\n".join(report) + "\n")
    assert not failures, failures
