"""Slice 1: answer status. With the drive build, the adapter must get
through detection: after each replug the drive sees a reset and the
adapter goes on to send commands, which it only does once status reads
look like a drive. Core 1 must also answer every read in time.

Exit criterion: past detection on 20 replugs in a row (with uhubctl;
by hand the default is one; override with HDDPRO_REPLUGS).
"""
from __future__ import annotations

import os
import re

import pytest

from conftest import require_build
from hddproto import trace

pytestmark = pytest.mark.slice1

# ATA lets IORDY hold a read for at most 1250 ns (tB). iordy_max_ns is the
# longest hold, measured on the bus by iordy_timer.pio.
IORDY_MAX_NS = 1250


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def test_adapter_gets_past_detection(drive, rig, run_dir):
    replugs = int(os.environ.get("HDDPRO_REPLUGS", 20 if rig.automatic_replug else 1))
    drive.send("r")  # reset the read timings
    for n in range(replugs):
        mark = drive.mark()
        rig.replug()
        drive.wait_for(lambda l: l.startswith(("RESET", "CMD")), since=mark, timeout=90)
        drive.wait_quiet(3, timeout=120)
        lines = drive.lines_since(mark)
        (run_dir / f"trace-{n + 1}.txt").write_text("\n".join(lines) + "\n")
        t = trace.parse(lines)
        print(f"\nreplug {n + 1}: " + trace.summary(t, sequence_lines=20))
        assert t.resets or any(c.command == 0x90 for c in t.commands), "no reset seen"
        assert t.commands, f"replug {n + 1}: the adapter sent no commands"
        assert not t.stalls and not t.dropped

    stats = drive.command("s", "STATS")
    iordy_ns = int(re.search(r"iordy_max_ns=(\d+)", stats).group(1))
    print(stats)
    assert iordy_ns < IORDY_MAX_NS, f"IORDY held a read for {iordy_ns} ns"
