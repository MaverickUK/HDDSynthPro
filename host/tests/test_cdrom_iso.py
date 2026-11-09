"""A CD-ROM drive (`device0 = cdrom` or `device1 = cdrom`) serving the test
ISO (docs/atapi.md, slice C2's exit): the host mounts the disc and every file
reads back as made.

Not yet run on the rig. Put the test ISO on the card first, as the disc the
settings name (HDDSYNTH.ISO by default):

    cd host && .venv/bin/python -m hddproto.test_iso build /Volumes/CARD/HDDSYNTH.ISO

The mounted disc: $HDDPRO_CDROM_MOUNT if set, else the volume labelled
HDDSYNTH_TEST (macOS: /Volumes/HDDSYNTH_TEST; Windows: its drive letter).
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

from conftest import cdrom_line
from hddproto import test_iso, trace

pytestmark = pytest.mark.cdrom


@pytest.fixture(scope="module", autouse=True)
def serving_the_iso(drive):
    line = cdrom_line(drive)
    if line is None:
        pytest.skip("needs a CD-ROM drive: `E device0 = cdrom` (or device1) and `!`")
    if "disc=test disc" in line or "disc=no disc" in line:
        pytest.skip(f"the drive isn't serving an ISO ({line.strip()}); build the test ISO onto the card "
                    "(python -m hddproto.test_iso build ...) and restart (!)")


def find_mount(timeout: float = 90.0) -> Path:
    wanted = os.environ.get("HDDPRO_CDROM_MOUNT")
    deadline = time.monotonic() + timeout
    while True:
        if wanted:
            if Path(wanted).is_dir():
                return Path(wanted)
        elif sys.platform == "darwin":
            if Path("/Volumes", test_iso.VOLUME).is_dir():
                return Path("/Volumes", test_iso.VOLUME)
        elif sys.platform == "win32":
            letter = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 f"(Get-Volume -FileSystemLabel '{test_iso.VOLUME}' -ErrorAction SilentlyContinue).DriveLetter"],
                capture_output=True, text=True).stdout.strip()
            if letter:
                return Path(f"{letter[0]}:\\")
        if time.monotonic() > deadline:
            raise TimeoutError(f"the disc {test_iso.VOLUME} wasn't mounted within {timeout:.0f} s "
                               "(set HDDPRO_CDROM_MOUNT to where it is)")
        time.sleep(1)


def test_every_file_reads_back(drive):
    mount = find_mount()
    mark = drive.mark()
    started = time.monotonic()
    problems = test_iso.check(mount)
    seconds = time.monotonic() - started
    assert problems == [], problems[:5]
    total = sum(len(d) for d in test_iso.expected().values())
    print(f"\n{len(test_iso.FILES)} files, {total / 1e6:.1f} MB checked in {seconds:.1f} s from {mount}")
    # The drive's view: its reads went without an error. (The host's cache may
    # have kept some frames from the mount, so not every byte needs a read.)
    drive.wait_quiet(1, timeout=30)
    packets = [c for c in trace.parse(drive.lines_since(mark)).commands if c.command == 0xA0]
    reads = [c for c in packets if c.name in ("READ_10", "READ_12")]
    assert all(c.ok for c in reads), [str(c) for c in reads if not c.ok][:5]
