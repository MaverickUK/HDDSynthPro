"""The CD-ROM build's test ISO (hddproto/test_iso.py), without the drive: it
builds, a mounted copy passes the check, and a damaged one doesn't. Mounting
needs macOS (hdiutil attach); elsewhere only the build runs, if an ISO maker
is installed."""
from __future__ import annotations

import plistlib
import shutil
import subprocess
import sys
import zlib
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


def test_iso9660_names_are_read_back(iso):
    names = test_iso.dos_names(iso.read_bytes())
    assert names["DATA/SUB/DEEPER/LAST.BIN"] == "DATA\\SUB\\DEEPER\\LAST.BIN"
    assert names["BIG5MB.BIN"] == "BIG5MB.BIN"


@pytest.mark.skipif(not shutil.which("nasm"), reason="CDCHECK.COM needs NASM")
def test_cdcheck_is_on_the_disc_with_every_dos_file(iso):
    on_disc = test_iso.iso9660_files(iso.read_bytes())
    assert "CDCHECK.COM" in on_disc
    names = test_iso.dos_names(iso.read_bytes())
    table = test_iso.files_inc(names)
    for path, data in test_iso.expected().items():
        if test_iso.is_dos_path(names[path]):
            assert f'"{names[path]}", 0' in table
            assert f"dd {len(data)}, 0x{zlib.crc32(data):08x}" in table


def test_dos_paths():
    assert test_iso.is_dos_path("DATA\\SUB\\DEEP.BIN")
    assert test_iso.is_dos_path("64K.BIN")
    assert test_iso.is_dos_path("README")
    assert not test_iso.is_dos_path("A LONG FILE NAME WITH SPAC.BIN")
    assert not test_iso.is_dos_path("TOOLONGNAME.BIN")
    assert not test_iso.is_dos_path("NAME.LONG")
