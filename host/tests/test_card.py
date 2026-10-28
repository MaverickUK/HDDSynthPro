"""The microSD card pulled out and put back while the drive runs (0.13.0).

The drive notices a card pulled out when an image read fails (`CARD lost`),
fails reads and writes at once until it's back, and serves the image again
(`CARD back`) only if the same card comes back with the image unchanged:
another card, or the image edited on a computer, would be corrupted by what
the host still has cached of the old one (`CARD changed`, not served).

Exit criteria:
  - pulled and put back while idle: the next reads succeed, the same data;
  - pulled during reads: `CARD lost`, then `CARD back` once it's in, and the
    host reads the same data again. If the card is back within about 20 s,
    the adapter's reset retries the failed read and it succeeds, so the host
    sees no error, only a slow read (seen on Windows, 28 Oct 2026);
  - with HDDPRO_SECOND_CARD=1, another card is refused (`CARD changed:
    another card`) until the right one is back.

Needs the drive build serving an image, and someone at the rig to pull the
card when asked. Reads only: safe on the owner's image. Run with -m card.
"""
from __future__ import annotations

import os
import re
import threading
import time

import pytest

from conftest import require_build
from hddproto.host import pread

pytestmark = pytest.mark.card

# A few places on the disk, read before and compared after.
SAMPLES = [0, 1, 63, 2048, 32768, 100000]
STATS_CARD = re.compile(r"card=(\S+) card_lost=(\d+) card_back=(\d+)")


@pytest.fixture(autouse=True)
def image_served(firmware_slice, drive):
    require_build(firmware_slice, drive_build=True)
    stats = drive.command("s", "STATS")
    if "card=present" not in stats:
        pytest.skip("the drive doesn't serve an image from the SD card (insert one, and restart the drive)")


def card_stats(drive) -> tuple[str, int, int]:
    m = STATS_CARD.search(drive.command("s", "STATS"))
    assert m, "no card= in the stats: firmware older than 0.13.0?"
    return m.group(1), int(m.group(2)), int(m.group(3))


def read_samples(fd) -> dict[int, bytes]:
    return {lba: pread(fd, 512, lba * 512) for lba in SAMPLES}


def reopen_and_read(rig, timeout: float = 240.0) -> dict[int, bytes]:
    """The samples, once the host can read the disk again: the adapter goes
    silent for about a minute after a failed read, then resets, and Windows
    may need the disk found again."""
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            disk = rig.find_disk(timeout=30)
            fd = rig.open_raw(disk)
            try:
                return read_samples(fd)
            finally:
                os.close(fd)
        except (OSError, TimeoutError) as e:
            last_error = e
            time.sleep(5)
    pytest.fail(f"the host couldn't read the disk again within {timeout:.0f} s: {last_error}")


@pytest.fixture
def before(rig):
    disk = rig.find_disk(timeout=30)
    fd = rig.open_raw(disk)
    try:
        return read_samples(fd)
    finally:
        os.close(fd)


def test_card_out_and_back_while_idle(drive, rig, ask, before):
    _, lost0, back0 = card_stats(drive)
    ask("Pull the microSD card out, count to three, and push it back in. You have 20 seconds.")
    time.sleep(20)
    ask("Thanks: reading the disk again.")
    assert reopen_and_read(rig) == before, "the image reads differently after the card came back"
    state, lost, back = card_stats(drive)
    assert state == "present"
    # A read that found the card restarted (pulled while idle) restarts it
    # and is retried: lost and back go up together, or neither if it wasn't
    # pulled at all.
    assert lost - lost0 == back - back0, f"lost {lost - lost0}, back {back - back0}"


def test_card_out_during_reads(drive, rig, ask, before):
    mark = drive.mark()
    disk = rig.find_disk(timeout=30)
    fd = rig.open_raw(disk)
    stop = threading.Event()
    errors = []

    def traffic():
        lba = 0
        while not stop.is_set():
            try:
                pread(fd, 64 << 10, lba * 512)
            except OSError as e:
                errors.append(e)
                time.sleep(1)
            lba = (lba + 128) % 120000

    reader = threading.Thread(target=traffic, daemon=True)
    reader.start()
    try:
        ask("Pull the microSD card out now (the drive is reading from it).")
        drive.wait_for(lambda l: l.startswith("CARD lost"), since=mark, timeout=120)
        assert card_stats(drive)[0] == "lost"
        ask("Seen. Now push the card back in.")
        drive.wait_for(lambda l: l.startswith("CARD back"), since=mark, timeout=120)
    finally:
        stop.set()
        reader.join(timeout=120)
        os.close(fd)
    # The host may not see an error at all: when the card is back before the
    # adapter's reset (about 20 s after the failed read), the adapter retries
    # the read and it succeeds, so the host only sees it take that long.
    print(f"read errors seen by the host while the card was out: {len(errors)}")
    ask("The drive has it again. Waiting for the host to read the disk (up to a few minutes).")
    assert reopen_and_read(rig) == before, "the image reads differently after the card came back"
    assert card_stats(drive)[0] == "present"


@pytest.mark.skipif(not os.environ.get("HDDPRO_SECOND_CARD"), reason="needs a second card: HDDPRO_SECOND_CARD=1")
def test_another_card_is_refused(drive, rig, ask, before):
    mark = drive.mark()
    ask("Pull the card out, and put the OTHER card in. You have 30 seconds.")
    time.sleep(30)
    # The drive only notices when a read fails: try one (it fails).
    try:
        disk = rig.find_disk(timeout=30)
        fd = rig.open_raw(disk)
        try:
            pread(fd, 512, 0)
        finally:
            os.close(fd)
    except OSError:
        pass
    line = drive.wait_for(lambda l: l.startswith("CARD changed"), since=mark, timeout=120)
    assert "another card" in line, line
    ask("Refused, as it should be. Now put the original card back in.")
    drive.wait_for(lambda l: l.startswith("CARD back"), since=mark, timeout=120)
    assert reopen_and_read(rig) == before
