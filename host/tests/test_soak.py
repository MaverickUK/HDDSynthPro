"""Slice 6: the soak. Hours of mixed traffic with every byte checked:
  - random reads anywhere on the generated disk, compared with its pattern;
  - random writes into the RAM overlay window, each read back, with a shadow
    copy of the window checked whole at the end;
  - every hour an 11-minute idle pause, with the raw device closed, long
    enough for the drive to be put in standby (macOS's disk sleep is 10
    minutes), so the next access has to wake it. The progress lines count
    standbys and wakes. HDDPRO_SOAK_IDLE_EVERY_S and _IDLE_PAUSE_S change it.

Every few minutes the drive's own view is checked as well: no failed
commands, no resets, no lost trace events, and IORDY never holding a read
past ATA's 1250 ns (tB). The first failure stops the soak; the drive's
trace and a progress log are left in host/runs/.

Writes use the same window as slice 4, so the overlay's 512 sectors are
never filled; the window gets its generated content back at the end.

Length: HDDPRO_SOAK_MINUTES (default 60). The exit criterion is overnight.
HDDPRO_SOAK_TOLERATE_RESETS=1 rides out host-side resets (macOS resetting the
adapter, as a USB glitch did once): logged, the read timings restarted after
the re-detection, the disk reopened, the write window re-checked. At most 3.
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
from hddproto.host import make_host, pread, pwrite

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
# Idle pauses, for standby and wake. macOS's disk sleep (pmset disksleep) is
# 10 minutes on the rig, so a pause has to be longer than that. Windows'
# "Turn off hard disk after" defaults to 20 minutes, and whether it reaches a
# USB disk at all is untested: lower it (powercfg /change disk-timeout-ac 10)
# or raise HDDPRO_SOAK_IDLE_PAUSE_S. The soak counts standbys, not requires them.
IDLE_EVERY_S = float(os.environ.get("HDDPRO_SOAK_IDLE_EVERY_S", 3600))
IDLE_PAUSE_S = float(os.environ.get("HDDPRO_SOAK_IDLE_PAUSE_S", 660))
TOLERATE_RESETS = os.environ.get("HDDPRO_SOAK_TOLERATE_RESETS") == "1"
MAX_RESETS = 3
DETECT_SETTLE_S = 10     # after a host reset, let the adapter detect the drive again
DISK_BACK_S = 180        # and wait this long for the disk to reappear
STANDBY_COMMANDS = (0xE0, 0xE2, 0x94, 0x96)  # STANDBY (IMMEDIATE), both opcodes
WAKE_COMMANDS = (0xE1, 0x95)                 # IDLE IMMEDIATE


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
    again = pread(fd, SECTOR, lba * SECTOR) == want
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
    standbys = sum(1 for c in t.commands if c.command in STANDBY_COMMANDS)
    wakes = sum(1 for c in t.commands if c.command in WAKE_COMMANDS)
    return (f"commands {len(t.commands)}, standbys {standbys}, wakes {wakes}, "
            f"IORDY max {iordy_ns} ns ({fields['iordy_max_reg']}), "
            f"early {fields.get('iordy_early', '?')} ({fields.get('iordy_early_reg', '-')}); "
            f"bus observer: {fields.get('bus_sectors', '?')} sectors checked, {fields.get('bus_bad', '?')} bad, "
            f"{fields.get('bus_gaps', '?')} gaps")


def on_the_bus(drive, start_mark: int, lba: int) -> str:
    """What the drive's bus observer saw of sector `lba`: whether the data
    was already wrong on the cable."""
    time.sleep(0.5)  # core 0 checks the observer's samples within a few ms
    hits = [l for l in drive.lines_since(start_mark) if l.startswith("BUSERR") and f" lba={lba} " in l]
    if hits:
        return f"wrong on the cable: {hits[-1]}"
    gaps = dict(re.findall(r"(\w+)=(\S+)", drive.command("s", "STATS"))).get("bus_gaps", "?")
    return f"right on the cable, as far as the observer saw (no BUSERR; bus_gaps={gaps})"


def reopen(disk, fd: int):
    """After the disk went away and came back: the raw device opened again on
    the same descriptor, so everything holding `fd` carries on."""
    deadline = time.monotonic() + DISK_BACK_S
    while True:
        try:
            reopened = make_host(print).open_raw(disk, write=True)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(5)
    os.dup2(reopened, fd)
    os.close(reopened)


def pause(disk, fd: int):
    """An idle pause, long enough for the drive to be put in standby, with
    the raw device closed so nothing on the Mac holds it. Meanwhile `fd`
    points at /dev/null rather than being freed: another thread could take
    a freed number. The device is then reopened on the same descriptor."""
    placeholder = os.open(os.devnull, os.O_RDONLY)
    os.dup2(placeholder, fd)  # closes the raw device
    os.close(placeholder)
    time.sleep(IDLE_PAUSE_S)
    reopened = make_host(print).open_raw(disk, write=True)
    os.dup2(reopened, fd)
    os.close(reopened)


def test_soak(raw_disk_rw, drive, run_dir):
    disk, fd = raw_disk_rw
    minutes = float(os.environ.get("HDDPRO_SOAK_MINUTES", "60"))
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    rng = random.Random(seed)
    log = open(run_dir / "soak.log", "w", encoding="utf-8")

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
    resets = 0

    def recover(reason: str):
        """A host-side reset (HDDPRO_SOAK_TOLERATE_RESETS=1): log it, wait for
        the re-detection, restart the read timings, re-check the window."""
        nonlocal resets, start_mark, shadow
        resets += 1
        note(f"HOST RESET {resets}: {reason}; waiting for the re-detection")
        assert resets <= MAX_RESETS, f"more than {MAX_RESETS} host resets"
        time.sleep(DETECT_SETTLE_S)
        reopen(disk, fd)
        drive.send("r")  # detection's reads (near 1.2 us) don't count
        start_mark = drive.mark()
        window = read(fd, WINDOW_START, WINDOW)
        bad = first_diff(window, bytes(shadow), WINDOW_START)
        if bad:
            note(f"the write window differs after the reset: {bad} (a write cut short?); taken as it is")
            shadow = bytearray(window)
        note("resumed")

    def soak_step() -> tuple[int, int, int]:
        """One random read, or one write read straight back: (reads, writes, sectors moved)."""
        if rng.random() < 0.65:  # a read anywhere outside the window
            count = rng.randint(1, 256)
            lba = rng.randrange(0, TOTAL - count + 1)
            if lba < WINDOW_START + WINDOW and lba + count > WINDOW_START:
                return 0, 0, 0
            data = read(fd, lba, count)
            bad = gen_disk.first_mismatch(data, lba)
            if bad is not None:
                i = (bad[0] - lba) * SECTOR
                errors.append(f"read {reads} (LBA {lba} x {count}): "
                              f"{describe(data[i:i + SECTOR], bad[0], fd)}; "
                              f"{on_the_bus(drive, start_mark, bad[0])}")
                note(f"MISMATCH {errors[-1]}")
                assert len(errors) < max_errors, f"{len(errors)} mismatches; last: {errors[-1]}"
            return 1, 0, count
        count = rng.randint(1, 32)  # a write into the window, read straight back
        offset = rng.randrange(0, WINDOW - count + 1)
        data = rng.randbytes(count * SECTOR)
        write(fd, WINDOW_START + offset, data)
        shadow[offset * SECTOR:(offset + count) * SECTOR] = data
        bad = first_diff(read(fd, WINDOW_START + offset, count), data, WINDOW_START + offset)
        assert bad is None, f"write {writes} (LBA {WINDOW_START + offset} x {count}): {bad}"
        return 0, 1, 2 * count

    try:
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= next_idle:
                pause(disk, fd)
                now = time.monotonic()
                next_idle = now + IDLE_EVERY_S
            if now >= next_check and TOLERATE_RESETS:
                t = trace.parse(drive.lines_since(start_mark))
                if t.resets:
                    first = t.resets[0][0]
                    failed = [str(c) for c in t.commands if c.status is not None and not c.ok and c.t_us < first]
                    assert not failed, f"failed commands before the reset: {failed[:5]}"
                    recover(f"{len(t.resets)} RESET# changes at the drive")
            if now >= next_check:
                summary = check_drive(drive, start_mark)
                elapsed = (now - started) / 60
                note(f"{elapsed:5.1f} min: {reads} reads, {writes} writes, "
                     f"{sectors_moved * SECTOR / 1e6:.0f} MB, {len(errors)} mismatches; {summary}")
                next_check = now + CHECK_EVERY_S

            try:
                moved = soak_step()
            except OSError as e:  # the disk went away: a host reset, if tolerated
                if not TOLERATE_RESETS:
                    raise
                recover(f"I/O error: {e}")
                continue
            reads += moved[0]
            writes += moved[1]
            sectors_moved += moved[2]

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
