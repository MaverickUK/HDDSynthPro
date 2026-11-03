"""The CD-ROM build's test ISO (hddproto/test_iso.py), without the drive: it
builds, a mounted copy passes the check, and a damaged one doesn't. Mounting
needs macOS (hdiutil attach); elsewhere only the build runs, if an ISO maker
is installed."""
from __future__ import annotations

import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from hddproto import test_iso

pytestmark = pytest.mark.sim


def have_iso_maker() -> bool:
    return sys.platform == "darwin" or any(shutil.which(t) for t in ("xorriso", "genisoimage", "mkisofs"))


@pytest.fixture(scope="module")
def iso(tmp_path_factory) -> Path:
    if not have_iso_maker():
        pytest.skip("no ISO maker (xorriso, genisoimage or mkisofs)")
    return test_iso.build(tmp_path_factory.mktemp("iso") / "HDDSYNTH.ISO")


def test_whole_frames(iso):
    assert iso.stat().st_size % 2048 == 0
    assert iso.stat().st_size > sum(test_iso.FILES.values())


@pytest.mark.skipif(sys.platform != "darwin", reason="mounting an ISO needs hdiutil")
def test_mounted_copy_checks_out(iso, tmp_path):
    out = subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse", "-plist", str(iso)],
                         capture_output=True, check=True).stdout
    entities = plistlib.loads(out)["system-entities"]
    mount = next(Path(e["mount-point"]) for e in entities if "mount-point" in e)
    try:
        assert test_iso.check(mount) == []
        # A copy with one byte wrong and one file missing is caught
        copy = tmp_path / "copy"
        shutil.copytree(mount, copy)
        big = copy / "BIG1MB.BIN"
        data = bytearray(big.read_bytes())
        data[700000] ^= 0x01
        big.chmod(0o644)
        big.write_bytes(bytes(data))
        (copy / "ONE.BIN").chmod(0o644)
        (copy / "ONE.BIN").unlink()
        problems = test_iso.check(copy)
        assert len(problems) == 2
        assert any("BIG1MB.BIN" in p and "byte 700000" in p for p in problems)
        assert any("ONE.BIN: missing" in p for p in problems)
    finally:
        subprocess.run(["hdiutil", "detach", str(mount)], capture_output=True)
