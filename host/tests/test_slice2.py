"""Slice 2: IDENTIFY DEVICE. After each replug, macOS sees a disk of the
right size, and the drive's trace shows the IDENTIFY command arriving.

Exit criterion: a disk of the right capacity on 20 replugs out of 20. That
many replugs needs uhubctl (HDDPRO_UHUBCTL); by hand, the default is one.
Override with HDDPRO_REPLUGS.
"""
from __future__ import annotations

import os

import pytest

from conftest import require_build
from hddproto import gen_disk, trace

pytestmark = pytest.mark.slice2


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


EXPECTED_BYTES = gen_disk.DEFAULT_SECTORS * gen_disk.SECTOR_BYTES
IDENTIFY_COMMAND = 0xEC


def replug_count(rig) -> int:
    if os.environ.get("HDDPRO_REPLUGS"):
        return int(os.environ["HDDPRO_REPLUGS"])
    return 20 if rig.automatic_replug else 1


def test_disk_appears_with_right_capacity(drive, rig, run_dir):
    for n in range(replug_count(rig)):
        mark = drive.mark()
        rig.replug()
        disk = rig.find_disk(EXPECTED_BYTES, timeout=90)
        assert disk.size == EXPECTED_BYTES, f"replug {n + 1}: {disk.name} is {disk.size} bytes"

        drive.wait_quiet(2, timeout=60)
        lines = drive.lines_since(mark)
        (run_dir / f"trace-{n + 1}.txt").write_text("\n".join(lines) + "\n")
        t = trace.parse(lines)
        identifies = [c for c in t.commands if c.command == IDENTIFY_COMMAND]
        assert identifies, f"replug {n + 1}: no IDENTIFY DEVICE in the trace"
        assert all(c.ok for c in identifies), f"replug {n + 1}: {identifies}"
        assert not t.stalls and not t.dropped
