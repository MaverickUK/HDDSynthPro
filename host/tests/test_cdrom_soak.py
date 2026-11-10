"""A CD-ROM soak: the generated test disc read for a long time in Doom's
pattern, small reads of 1-3 frames scattered over the disc with now and then
a longer run, every byte checked here and, with the bus observer on full
checks, every sector on the cable too. Written for the Evesham's wrong word
(10 Nov 2026, docs/atapi.md): one data word in about 5.8 million, the word
before it again.

Through the rig's USB adapter a CD-ROM's data goes by multiword DMA, since
the adapter sets the DMA bit on every PACKET (docs/atapi.md; `mode = mdma2`),
so this soaks the CD-ROM's commands, its read-ahead and the card, but not
ide_bus's PIO data path, where the Evesham's word went wrong; the test says
which it ran on. A host that reads a CD-ROM by PIO (the Evesham under DOS, a
native PATA host) is what reproduces that.

It needs the test disc as device 1 (`E device1 = cdrom`, `E cdrom = none`, `!`,
or console `T` with `device1 = cdrom`), like test_cdrom.py. With
$HDDPRO_DISK_DEV (the hard disk's raw device) it also reads device 0, the
test disk, between the CD's reads, as the Evesham read C: and D:.

Length: HDDPRO_SOAK_MINUTES (default 60). HDDPRO_SEED repeats a run.
"""
from __future__ import annotations

import os
import random
import re
import sys
import time

import pytest

from conftest import cdrom_device, require_generated_disk
from hddproto import gen_disk
from hddproto.host import make_host, pread

pytestmark = pytest.mark.cdsoak

FRAME = 2048
SECTORS_PER_FRAME = FRAME // gen_disk.SECTOR_BYTES
FRAMES = gen_disk.DEFAULT_SECTORS // SECTORS_PER_FRAME
SECTOR = gen_disk.SECTOR_BYTES
CHECK_EVERY_S = 60


def stats(drive) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=(\S+)", drive.command("s", "STATS")))


@pytest.fixture(scope="module")
def disc(drive):
    if cdrom_device(drive) != 1:
        pytest.skip("needs the CD-ROM drive as device 1: `E device1 = cdrom`, then `!`")
    require_generated_disk(drive)
    path = os.environ.get("HDDPRO_CDROM_DEV")
    if path:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
    elif sys.platform == "darwin":
        rig = make_host(lambda m: print(f"\n>>> {m}"))
        fd = rig.open_raw(rig.find_disk(FRAMES * FRAME))
    else:
        pytest.skip("set HDDPRO_CDROM_DEV to the drive's raw device (e.g. \\\\.\\CdRom0)")
    yield fd
    os.close(fd)


@pytest.fixture(scope="module")
def hard_disk():
    """Device 0's raw device, read-only, if $HDDPRO_DISK_DEV names it."""
    path = os.environ.get("HDDPRO_DISK_DEV")
    if not path:
        yield None
        return
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
    yield fd
    os.close(fd)


def test_cdrom_soak(disc, hard_disk, drive, run_dir):
    minutes = float(os.environ.get("HDDPRO_SOAK_MINUTES", "60"))
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    rng = random.Random(seed)
    log = open(run_dir / "cdsoak.log", "w", encoding="utf-8")

    def note(line: str):
        stamped = f"{time.strftime('%H:%M:%S')} {line}"
        print(stamped, flush=True)
        log.write(stamped + "\n")
        log.flush()

    note(f"CD-ROM soak for {minutes:g} min, seed {seed} (rerun with HDDPRO_SEED={seed})"
         + ("; device 0 read too" if hard_disk is not None else ""))
    if "bus_checks=full" not in drive.command("s", "STATS").split():
        assert drive.command("O", "OBSERVER").endswith("full"), "console O didn't switch the observer"
    drive.send("r")  # read timings and bus counters from here
    mark = drive.mark()
    started = time.monotonic()
    end, next_check = started + minutes * 60, started + CHECK_EVERY_S
    reads = frames = disk_reads = 0
    errors: list[str] = []
    try:
        while time.monotonic() < end:
            # Doom's pattern: mostly 1-3 frames anywhere, sometimes a run.
            count = rng.choice((1, 1, 2, 3, 3)) if rng.random() < 0.9 else rng.randint(8, 32)
            frame = rng.randrange(0, FRAMES - count + 1)
            data = pread(disc, count * FRAME, frame * FRAME)
            reads, frames = reads + 1, frames + count
            bad = gen_disk.first_mismatch(data, frame * SECTORS_PER_FRAME)
            if bad is not None:
                errors.append(f"read {reads} (frame {frame} x {count}): sector {bad[0]}, byte {bad[1]}")
                note(f"MISMATCH {errors[-1]}")
            if hard_disk is not None and rng.random() < 0.2:  # C: between D:'s reads
                n = rng.randint(1, 16)
                lba = rng.randrange(0, gen_disk.DEFAULT_SECTORS - n + 1)
                bad = gen_disk.first_mismatch(pread(hard_disk, n * SECTOR, lba * SECTOR), lba)
                disk_reads += 1
                if bad is not None:
                    errors.append(f"device 0 read (LBA {lba} x {n}): sector {bad[0]}, byte {bad[1]}")
                    note(f"MISMATCH {errors[-1]}")
            if time.monotonic() >= next_check:
                next_check += CHECK_EVERY_S
                s = stats(drive)
                note(f"{reads} reads, {frames} frames, {disk_reads} device 0 reads; "
                     f"bus_sectors={s.get('bus_sectors')} bus_bad={s.get('bus_bad')} "
                     f"iordy_ignored={s.get('iordy_ignored')} bus_dma_words={s.get('bus_dma_words')}")
    finally:
        lines = drive.lines_since(mark)
        flagged = [l for l in lines if l.startswith(("BUSERR", "NOIORDY", "STUCK", "CARD"))]
        for line in flagged:
            note(f"drive: {line}")
        s = stats(drive)
        path = "multiword DMA" if int(s.get("bus_dma_words", "0")) else "PIO (ide_bus's data path)"
        note(f"done: {reads} CD reads ({frames} frames), {disk_reads} device 0 reads, {len(errors)} wrong; "
             f"the CD's data went by {path}; bus_sectors={s.get('bus_sectors')} bus_bad={s.get('bus_bad')} "
             f"bus_gaps={s.get('bus_gaps')} iordy_ignored={s.get('iordy_ignored')} "
             f"iordy_max_ns={s.get('iordy_max_ns')} card_lost={s.get('card_lost')}")
        log.close()
    assert not errors, f"{len(errors)} wrong reads; first: {errors[0]}"
    assert s.get("bus_bad") == "0", f"the bus observer saw {s.get('bus_bad')} bad sectors: {flagged[:3]}"
    assert s.get("iordy_ignored") in ("0", "n/a", None)
