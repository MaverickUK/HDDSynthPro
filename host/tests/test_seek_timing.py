"""Authentic seek timing (seek_timing in HDDSYNTH.INI, src/seek_timing.*):
does the host really run at the modelled drive's pace?

- Random 4 KB reads (as the activity test's random phase) take about
  seek + half a turn + the transfer each: the host's rate is within 20% of
  the drive's own modelled time for the same commands (`s`: seek_total_us
  over seek_commands).
- Sequential reads run at about the modelled media rate (63 sectors a turn,
  identify's default geometry), or the host's own rate if that's slower.
- With timing switched off (console `H`), reads run as fast as before.

The model itself (seek times, rotation) is held to the HDD Synth's engine by
HDD Synth's host tests, and the scheduler by test/native/test_seek_timing.cpp.

Needs the drive build with `seek_timing` set to a preset; voice97 is the one
the brief measures (`E seek_timing = voice97`, then `!`, and `E seek_timing =
off`, `!` afterwards: the other rig tests run with it off). Reads only.
"""
from __future__ import annotations

import os
import random
import re
import time

import pytest

from conftest import require_build
from hddproto import gen_disk
from hddproto.host import pread

pytestmark = pytest.mark.seek

SECTOR = gen_disk.SECTOR_BYTES
SECTORS_PER_TRACK = 63  # identify's default geometry (16 heads x 63)
RANDOM_READS = 300
SEQ_BYTES = 8 << 20
REQUEST = 64 << 10
TOLERANCE = 0.20


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def stats(drive) -> dict:
    return dict(re.findall(r"(\w+)=(\S+)", drive.command("s", "STATS")))


def set_timing(drive, on: bool):
    line = drive.command("H", "SEEKTIMING")
    if (line.split()[1] == "on") != on:
        line = drive.command("H", "SEEKTIMING")
    assert line.split()[1] == ("on" if on else "off"), line
    time.sleep(0.01)  # the model is picked up once a millisecond


@pytest.fixture
def timed(drive):
    s = stats(drive)
    if s.get("seek_timing") in (None, "off", "synth"):
        pytest.skip(f"seek_timing is {s.get('seek_timing')}: set a preset (E seek_timing = voice97, then !)")
    set_timing(drive, True)
    yield s
    set_timing(drive, True)  # as the setting says, for whatever runs next


def run(drive, work) -> tuple[float, int, int]:
    """Runs `work`; returns its time, and the commands and modelled time the
    drive counted meanwhile."""
    before = stats(drive)
    start = time.perf_counter()
    work()
    elapsed = time.perf_counter() - start
    after = stats(drive)
    commands = int(after["seek_commands"]) - int(before["seek_commands"])
    modelled_us = int(after["seek_total_us"]) - int(before["seek_total_us"])
    return elapsed, commands, modelled_us


def random_reads(fd, rng, total):
    for _ in range(RANDOM_READS):
        pread(fd, 8 * SECTOR, rng.randrange(0, total - 8) * SECTOR)


def test_random_reads_at_the_modelled_pace(drive, raw_disk, timed):
    _, fd = raw_disk
    total = int(timed["sectors"])
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    rng = random.Random(seed)
    lbas = [rng.randrange(0, total - 8) for _ in range(RANDOM_READS)]

    def work():
        for lba in lbas:
            pread(fd, 8 * SECTOR, lba * SECTOR)

    elapsed, commands, modelled_us = run(drive, work)
    assert commands == RANDOM_READS, f"{commands} commands timed, not {RANDOM_READS} (seed {seed})"
    modelled_ms = modelled_us / commands / 1000
    measured_ms = elapsed / RANDOM_READS * 1000
    print(f"\nseek timing {timed['seek_timing']}: random 4 KB reads {measured_ms:.2f} ms each, "
          f"modelled {modelled_ms:.2f} ms (seed {seed})")
    # voice97: about 19 ms (a third-of-the-stroke seek, half of 11.1 ms, 8 sectors).
    assert 3 < modelled_ms < 600, f"modelled {modelled_ms:.2f} ms a read: not a drive"
    assert abs(measured_ms / modelled_ms - 1) <= TOLERANCE, \
        f"random reads took {measured_ms:.2f} ms each, the model says {modelled_ms:.2f} (seed {seed})"
    s = stats(drive)
    assert int(s["seek_capped"]) == 0, "a command hit the 1 s cap"


def test_sequential_reads_at_the_media_rate(drive, raw_disk, timed):
    _, fd = raw_disk
    start_lba = 4096

    def sequential():
        for offset in range(0, SEQ_BYTES, REQUEST):
            pread(fd, REQUEST, start_lba * SECTOR + offset)

    set_timing(drive, False)
    host_elapsed = run(drive, sequential)[0]
    set_timing(drive, True)
    pread(fd, SECTOR, int(timed["sectors"]) // 2 * SECTOR)  # the head elsewhere first
    elapsed = run(drive, sequential)[0]

    rpm = int(stats(drive)["seek_rpm"])
    media = SECTORS_PER_TRACK * SECTOR * rpm / 60 / 1e6  # MB/s
    host = SEQ_BYTES / host_elapsed / 1e6
    measured = SEQ_BYTES / elapsed / 1e6
    expected = min(media, host)
    print(f"\nsequential 64 KB reads: {measured:.2f} MB/s with timing, media rate {media:.2f}, "
          f"host without timing {host:.2f}")
    assert measured <= media * (1 + TOLERANCE), f"{measured:.2f} MB/s: faster than the platter ({media:.2f})"
    assert measured >= expected * (1 - TOLERANCE), \
        f"{measured:.2f} MB/s, expected about {expected:.2f} (media {media:.2f}, host {host:.2f})"


def test_off_runs_as_before(drive, raw_disk, timed):
    _, fd = raw_disk
    total = int(timed["sectors"])
    rng = random.Random(1)
    set_timing(drive, False)
    before = stats(drive)
    start = time.perf_counter()
    random_reads(fd, rng, total)
    off_ms = (time.perf_counter() - start) / RANDOM_READS * 1000
    after = stats(drive)
    print(f"\nrandom 4 KB reads with timing off: {off_ms:.2f} ms each")
    assert after["seek_active"] == "0"
    assert after["seek_commands"] == before["seek_commands"], "commands were timed with timing off"
    assert off_ms < 3, f"{off_ms:.2f} ms a read with timing off: something still holds them"
