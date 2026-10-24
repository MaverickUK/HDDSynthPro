"""Slice 6: the soak. Hours of mixed traffic with every byte checked:
  - random reads anywhere on the generated disk, compared with its pattern;
  - random writes into the RAM overlay window, each read back, with a shadow
    copy of the window checked whole at the end;
  - now and then an idle pause long enough for the adapter to put the drive
    in standby, so the next access has to wake it.

Every few minutes the drive's own view is checked as well: no failed
commands, no resets, no lost trace events, and IORDY never holding a read
past ATA's 1250 ns (tB). The first failure stops the soak; the drive's
trace and a progress log are left in host/runs/.

Writes use the same window as slice 4, so the overlay's 512 sectors are
never filled; the window gets its generated content back at the end.

Length: HDDPRO_SOAK_MINUTES (default 60). The exit criterion is overnight.
A mismatch is diagnosed in the log (whose data came back, shifted or not,
whether a re-read is right). HDDPRO_SOAK_MAX_ERRORS (default 1) lets a
diagnostic run carry on and collect several.
Exit criterion: an overnight soak with zero mismatches and zero hangs.
"""
from __future__ import annotations

import os
import random
import re
import time

import pytest

from conftest import require_build, require_generated_disk
from hddproto import gen_disk, trace

pytestmark = pytest.mark.slice6


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


@pytest.fixture(scope="module", autouse=True)
def generated_disk(drive):
    require_generated_disk(drive)  # before the raw disk is opened


SECTOR = gen_disk.SECTOR_BYTES
TOTAL = gen_disk.DEFAULT_SECTORS
WINDOW_START = 100_000  # as in test_slice4.py
WINDOW = 256
IORDY_MAX_NS = 1250
CHECK_EVERY_S = 300     # the drive's view
IDLE_EVERY_S = 600      # a pause for the adapter's standby
IDLE_PAUSE_S = 60       # it sends STANDBY after about 40 s of quiet


def read(fd: int, lba: int, count: int) -> bytes:
    data = os.pread(fd, count * SECTOR, lba * SECTOR)
    assert len(data) == count * SECTOR, f"short read at LBA {lba}: {len(data)} bytes"
    return data


def write(fd: int, lba: int, data: bytes):
    assert os.pwrite(fd, data, lba * SECTOR) == len(data), f"short write at LBA {lba}"


def first_diff(got: bytes, want: bytes, lba: int) -> str | None:
    for i, (a, b) in enumerate(zip(got, want)):
        if a != b:
            return f"LBA {lba + i // SECTOR}, byte {i % SECTOR}: got {a:#04x}, want {b:#04x}"
    return None


def describe(got: bytes, lba: int, fd: int) -> str:
    """What came back instead of generated sector `lba`."""
    want = gen_disk.sector(lba)
    diffs = [i for i in range(SECTOR) if got[i] != want[i]]
    held = int.from_bytes(got[0:4], "little")
    if got == bytes(SECTOR):
        whose = "all zeros"
    elif int.from_bytes(got[4:8], "little") == (~held & 0xFFFFFFFF) and got == gen_disk.sector(held):
        whose = f"generated sector {held} ({held - lba:+d})"
    elif got[2:] == want[:-2]:
        whose = "the right sector, one word late"
    elif got[:-2] == want[2:]:
        whose = "the right sector, one word early"
    else:
        whose = f"unrecognised, starts {got[:16].hex()}"
    again = os.pread(fd, SECTOR, lba * SECTOR) == want
    return (f"LBA {lba}: {len(diffs)} bytes differ (first at {diffs[0]}), holds {whose}; "
            f"re-read {'right' if again else 'still wrong'}")


def check_drive(drive, start_mark: int) -> str:
    """Checks the drive's stats and trace since the soak began; returns a
    one-line summary for the log."""
    stats = drive.command("s", "STATS")
    fields = dict(re.findall(r"(\w+)=(\S+)", stats))
    assert int(fields["dropped"]) == 0 and int(fields["stalls"]) == 0, f"trace lost events: {stats}"
    iordy_ns = int(fields["iordy_max_ns"])
    assert iordy_ns < IORDY_MAX_NS, \
        f"IORDY held a {fields['iordy_max_reg']} read for {iordy_ns} ns: {stats}"
    t = trace.parse(drive.lines_since(start_mark))
    assert not t.resets, f"the host reset the drive: {t.resets[:4]}"
    failed = [str(c) for c in t.commands if c.status is not None and not c.ok]
    assert not failed, f"failed commands: {failed[:5]}"
    wakes = sum(1 for c in t.commands if c.command in (0xE1, 0x95))
    return f"commands {len(t.commands)}, wakes {wakes}, IORDY max {iordy_ns} ns ({fields['iordy_max_reg']})"


def test_soak(raw_disk_rw, drive, run_dir):
    _, fd = raw_disk_rw
    minutes = float(os.environ.get("HDDPRO_SOAK_MINUTES", "60"))
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    rng = random.Random(seed)
    log = open(run_dir / "soak.log", "w")

    def note(line: str):
        stamped = f"{time.strftime('%H:%M:%S')} {line}"
        print(stamped, flush=True)
        log.write(stamped + "\n")
        log.flush()

    note(f"soak for {minutes:g} min, seed {seed} (rerun with HDDPRO_SEED={seed})")
    drive.send("r")  # read timings and bus counters from here
    start_mark = drive.mark()
    shadow = bytearray(read(fd, WINDOW_START, WINDOW))
    started = time.monotonic()
    end = started + minutes * 60
    next_check = started + CHECK_EVERY_S
    next_idle = started + IDLE_EVERY_S
    reads = writes = sectors_moved = 0
    max_errors = int(os.environ.get("HDDPRO_SOAK_MAX_ERRORS", "1"))
    errors: list[str] = []
    try:
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= next_idle:
                time.sleep(IDLE_PAUSE_S)
                next_idle = time.monotonic() + IDLE_EVERY_S
            if now >= next_check:
                summary = check_drive(drive, start_mark)
                elapsed = (now - started) / 60
                note(f"{elapsed:5.1f} min: {reads} reads, {writes} writes, "
                     f"{sectors_moved * SECTOR / 1e6:.0f} MB, {len(errors)} mismatches; {summary}")
                next_check = now + CHECK_EVERY_S

            if rng.random() < 0.65:  # a read anywhere outside the window
                count = rng.randint(1, 256)
                lba = rng.randrange(0, TOTAL - count + 1)
                if lba < WINDOW_START + WINDOW and lba + count > WINDOW_START:
                    continue
                data = read(fd, lba, count)
                bad = gen_disk.first_mismatch(data, lba)
                if bad is not None:
                    i = (bad[0] - lba) * SECTOR
                    errors.append(f"read {reads} (LBA {lba} x {count}): "
                                  f"{describe(data[i:i + SECTOR], bad[0], fd)}")
                    note(f"MISMATCH {errors[-1]}")
                    assert len(errors) < max_errors, f"{len(errors)} mismatches; last: {errors[-1]}"
                reads += 1
                sectors_moved += count
            else:  # a write into the window, read straight back
                count = rng.randint(1, 32)
                offset = rng.randrange(0, WINDOW - count + 1)
                data = rng.randbytes(count * SECTOR)
                write(fd, WINDOW_START + offset, data)
                shadow[offset * SECTOR:(offset + count) * SECTOR] = data
                bad = first_diff(read(fd, WINDOW_START + offset, count), data, WINDOW_START + offset)
                assert bad is None, f"write {writes} (LBA {WINDOW_START + offset} x {count}): {bad}"
                writes += 1
                sectors_moved += 2 * count

        bad = first_diff(read(fd, WINDOW_START, WINDOW), bytes(shadow), WINDOW_START)
        assert bad is None, f"window doesn't match what was written: {bad}"
        assert not errors, f"{len(errors)} mismatches; first: {errors[0]}"
        summary = check_drive(drive, start_mark)
        note(f"done after {(time.monotonic() - started) / 60:.1f} min: {reads} reads, {writes} writes, "
             f"{sectors_moved * SECTOR / 1e6:.0f} MB, no mismatches; {summary}")
    except Exception as e:
        note(f"FAILED after {(time.monotonic() - started) / 60:.1f} min: {e}")
        raise
    finally:
        write(fd, WINDOW_START, gen_disk.sectors(WINDOW_START, WINDOW))
        log.close()
