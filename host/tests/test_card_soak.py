"""A soak of the SD card path: the drive serving the scratch image
HDDBENCH.IMG (console `J`), written and read for hours, every byte checked.

The ordinary soak (test_soak.py) needs the generated disk, whose contents it
can predict; this one makes the image predictable instead. Each sector it
writes holds its own LBA and a generation number, repeated, so a read that
returns another sector, or an older write of this one, is caught and named.
It starts by writing the whole image (64 MB), then mixes writes of random
runs (a new generation) with reads of random runs, checked against what was
last written there, and a full read-back every HDDPRO_CARD_SOAK_SWEEP_MIN
minutes (default 30). Made for the PCB build's SDIO (pcb_drive), where the
fastest set-up (an image with DMA) runs without the bus observer, so this is
what checks it; it runs on SPI just the same.

Only ever on HDDBENCH.IMG, the scratch image for benchmarks that write: it
skips unless the drive says `disk=bench`. Length: HDDPRO_SOAK_MINUTES (default
60). Every 5 minutes it logs the throughput and the drive's card counters
(`card_lost`, `card_restarts`); a card lost or restarted fails it, as does any
mismatch. Evidence: card_soak.log in the run folder.
"""
from __future__ import annotations

import os
import random
import re
import struct
import time

import pytest

from hddproto.host import make_host, pread, pwrite

pytestmark = pytest.mark.cardsoak

SECTOR = 512
MAX_RUN = 128  # sectors per command, as a host's 64 KB transfers
SWEEP_MIN = float(os.environ.get("HDDPRO_CARD_SOAK_SWEEP_MIN", "30"))


def sector_bytes(lba: int, generation: int) -> bytes:
    return struct.pack("<II", lba, generation) * (SECTOR // 8)


def run_bytes(lba: int, count: int, generation: int) -> bytes:
    return b"".join(sector_bytes(lba + i, generation) for i in range(count))


def stats(drive) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=(\S+)", drive.command("s", "STATS")))


@pytest.fixture
def bench_image(drive):
    """The raw device, opened for writing, only while the drive serves HDDBENCH.IMG."""
    s = stats(drive)
    if s.get("disk") != "bench":
        pytest.skip("the drive must serve the scratch image HDDBENCH.IMG: console J (it restarts; then replug the adapter)")
    sectors = int(s["sectors"])
    rig = make_host(lambda m: print(f"\n>>> {m}"))
    disk = rig.find_disk(sectors * SECTOR)
    fd = rig.open_raw(disk, write=True)
    yield fd, sectors
    os.close(fd)


def test_card_soak(bench_image, drive, run_dir):
    fd, sectors = bench_image
    minutes = float(os.environ.get("HDDPRO_SOAK_MINUTES", "60"))
    seed = int(os.environ.get("HDDPRO_SEED", time.time_ns() % 2**32))
    rng = random.Random(seed)
    log = open(run_dir / "card_soak.log", "w", encoding="utf-8")

    def note(line: str):
        stamped = f"{time.strftime('%H:%M:%S')} {line}"
        print(stamped, flush=True)
        log.write(stamped + "\n")
        log.flush()

    before = stats(drive)
    note(f"card soak for {minutes:g} min on {sectors} sectors, seed {seed} (rerun with HDDPRO_SEED={seed}); "
         f"card_bus={before.get('card_bus', '?')} mode_used={before.get('mode_used', '?')} diag={before.get('diag', '?')}")
    lost0, restarts0 = int(before.get("card_lost", 0)), int(before.get("card_restarts", 0))

    generation = [1] * sectors  # what each sector should hold
    written = read = 0

    def write_run(lba: int, count: int, gen: int):
        nonlocal written
        assert pwrite(fd, run_bytes(lba, count, gen), lba * SECTOR) == count * SECTOR
        for i in range(count):
            generation[lba + i] = gen
        written += count

    def check_run(lba: int, count: int):
        nonlocal read
        data = pread(fd, count * SECTOR, lba * SECTOR)
        assert len(data) == count * SECTOR
        read += count
        for i in range(count):
            got = data[i * SECTOR:(i + 1) * SECTOR]
            want = sector_bytes(lba + i, generation[lba + i])
            if got != want:
                held_lba, held_gen = struct.unpack("<II", got[:8])
                whole = got == sector_bytes(held_lba, held_gen)
                note(f"MISMATCH at LBA {lba + i}: want generation {generation[lba + i]}, got "
                     + (f"LBA {held_lba} generation {held_gen}" if whole else f"other data ({got[:16].hex()}...)"))
                pytest.fail(f"LBA {lba + i} read back wrong (see card_soak.log)")

    # Fill the image, so every sector has a known generation.
    t = time.monotonic()
    for lba in range(0, sectors, MAX_RUN):
        write_run(lba, min(MAX_RUN, sectors - lba), 1)
    note(f"filled in {time.monotonic() - t:.1f} s")

    start = time.monotonic()
    end = start + minutes * 60
    next_report = start + 300
    next_sweep = start + SWEEP_MIN * 60
    gen = 1
    last_written = last_read = 0
    last_t = start
    while time.monotonic() < end:
        count = rng.choice((1, 2, 8, 16, 64, 128, rng.randint(1, MAX_RUN)))
        lba = rng.randrange(0, sectors - count + 1)
        if rng.random() < 0.4:
            gen += 1
            write_run(lba, count, gen)
        else:
            check_run(lba, count)
        now = time.monotonic()
        if now >= next_sweep:
            t = now
            for at in range(0, sectors, MAX_RUN):
                check_run(at, min(MAX_RUN, sectors - at))
            note(f"full read-back: all {sectors} sectors right ({time.monotonic() - t:.1f} s)")
            next_sweep = time.monotonic() + SWEEP_MIN * 60
        if now >= next_report:
            s = stats(drive)
            span = now - last_t
            note(f"{(now - start) / 60:.0f} min: wrote {(written - last_written) * SECTOR / span / 1e6:.2f} MB/s, "
                 f"read {(read - last_read) * SECTOR / span / 1e6:.2f} MB/s; total {written * SECTOR / 1e9:.2f} GB "
                 f"written, {read * SECTOR / 1e9:.2f} GB read; card={s.get('card')} card_lost={s.get('card_lost')} "
                 f"card_restarts={s.get('card_restarts')}")
            assert int(s.get("card_lost", 0)) == lost0 and int(s.get("card_restarts", 0)) == restarts0, \
                "the card was lost or restarted (see the drive's CARD lines)"
            last_written, last_read, last_t = written, read, now
            next_report = now + 300

    for at in range(0, sectors, MAX_RUN):
        check_run(at, min(MAX_RUN, sectors - at))
    after = stats(drive)
    note(f"done: {written * SECTOR / 1e9:.2f} GB written, {read * SECTOR / 1e9:.2f} GB read, every sector checked; "
         f"card_lost={after.get('card_lost')} card_restarts={after.get('card_restarts')}")
    assert int(after.get("card_lost", 0)) == lost0 and int(after.get("card_restarts", 0)) == restarts0
