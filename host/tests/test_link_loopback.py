"""The activity link's loopback test: with a jumper from GPIO 0 (the link's
TX) to GPIO 1 (RX), console `U` reads back what the link sends for a second
($HDA and $HDE) and checks it with the same rules as a receiver. It also
sends one $HDM itself, as the synth would, and checks the receive side reads
it back as it went. No HDD Synth needed.

Exit criterion: every sentence sent while listening arrives valid, with no
UART errors, idle and during traffic, and the $HDM comes back.

Needs the drive build and the jumper; run with -m link. Reads only.
"""
from __future__ import annotations

import random
import re
import threading
import time

import pytest

from conftest import require_build
from hddproto import activity_link
from hddproto.host import pread

pytestmark = pytest.mark.link

LOOPBACK = re.compile(r"LOOPBACK (\S+) sent=(\d+) valid=(\d+) rejected=(\d+) errors=(\d+) bytes=(\d+) last=(\S+)"
                      r" head_sent=(\d+) head_valid=(\d+) model=(\S+)")


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def loopback(drive) -> dict:
    line = drive.command("U", "LOOPBACK", timeout=5)
    m = LOOPBACK.match(line)
    assert m, line
    result, sent, valid, rejected, errors, nbytes, last, head_sent, head_valid, model = m.groups()
    if result == "no-data":
        pytest.fail("nothing arrived on GPIO 1: fit a jumper from GPIO 0 to GPIO 1 (EXT1 pins 4 and 5)")
    return dict(line=line, result=result, sent=int(sent), valid=int(valid), rejected=int(rejected),
                errors=int(errors), bytes=int(nbytes), last=last, head_sent=int(head_sent),
                head_valid=int(head_valid), model=model)


def check(r: dict):
    assert r["result"] == "pass", r["line"]
    assert r["sent"] >= 5, f"fewer sentences than the heartbeat alone sends: {r['line']}"
    assert activity_link.parse(r["last"]) is not None, r["line"]
    assert r["head_valid"] == r["head_sent"], r["line"]
    assert r["model"] == "ok", f"the $HDM sent didn't come back as it went: {r['line']}"


def test_loopback_idle(drive):
    for _ in range(3):  # repeated: each run must start clean
        check(loopback(drive))


def test_loopback_during_reads(drive, raw_disk):
    _, fd = raw_disk  # read-only
    stop = threading.Event()

    def traffic():
        # Sequential bursts and random reads, so busy and the class change
        # and the link sends more than its heartbeat.
        while not stop.is_set():
            base = random.randrange(0, 100_000) * 512
            for i in range(8):
                pread(fd, 64 << 10, base + i * (64 << 10))
            for _ in range(20):
                pread(fd, 4096, random.randrange(0, 120_000) * 512)

    t = threading.Thread(target=traffic, daemon=True)
    t.start()
    try:
        time.sleep(1)
        results = [loopback(drive) for _ in range(3)]
    finally:
        stop.set()
        t.join(timeout=10)
    for r in results:
        print(r["line"])
        check(r)
    assert any(activity_link.parse(r["last"]).busy for r in results), "the link never said busy during reads"
    assert sum(r["head_sent"] for r in results) > 0, "no head targets ($HDE) went out during reads"
