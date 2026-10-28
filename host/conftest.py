"""Shared fixtures for the rig tests.

  drive    the drive's serial log (opened once per session, waits for READY)
  rig      the host side: replug the adapter, system log (macOS or Windows)
  run_dir  a timestamped folder under host/runs/ for this test's evidence;
           on failure the drive log, system log and disk list land there too
"""
from __future__ import annotations

import os
import sys
import time
from datetime import datetime

import pytest

from hddproto import pio_sim
from hddproto.drive_log import DriveLog
from hddproto.host import make_host
from hddproto.paths import REPO, RUNS


def pytest_addoption(parser):
    parser.addoption("--save-reference", action="store_true",
                     help="save this run's traces as the new references in host/reference/")


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    report = yield
    setattr(item, f"report_{report.when}", report)
    return report


@pytest.fixture(scope="session")
def assemble():
    """Assembles src/<name>.pio with the real pioasm. Skips without pioasm,
    except in CI, where a missing pioasm must not hide these tests."""
    pioasm = pio_sim.find_pioasm()
    if not pioasm:
        message = "pioasm not found (build once with `pio run`, or set PIOASM)"
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)
    return lambda name, program=None: pio_sim.assemble(REPO / "src" / f"{name}.pio", pioasm, program)


@pytest.fixture(scope="session")
def session_stamp():
    return datetime.now().strftime("%Y%m%d-%H%M%S")


@pytest.fixture(scope="session")
def drive(session_stamp):
    RUNS.mkdir(parents=True, exist_ok=True)
    log = DriveLog(raw_log_path=RUNS / f"{session_stamp}-serial.log")
    log.open()
    log.ready = log.command("h", "READY", timeout=5)  # "READY HDDSynthPro <version> slice<N>"
    yield log
    log.close()


@pytest.fixture
def firmware_slice(drive) -> int:
    """Which build the board runs: 0 = listen only, 3 = the drive (slices 1-5)."""
    return int(drive.ready.split()[-1].removeprefix("slice"))


def require_build(firmware_slice: int, drive_build: bool):
    """Skips unless the board runs the build the test needs."""
    if drive_build and firmware_slice == 0:
        pytest.skip("needs the drive build: PIO_ENV=bb48_drive scripts/rig.sh ...")
    if not drive_build and firmware_slice != 0:
        pytest.skip("needs the listen-only build: scripts/rig.sh ... (PIO_ENV=bb48)")


def require_generated_disk(drive):
    """Skips unless the drive serves the generated disk. Tests that compare
    reads with its pattern, or write to it, must never run on an SD image
    (slice 5): their writes would corrupt the filesystem on it."""
    stats = drive.command("s", "STATS")
    if "disk=generated" not in stats:
        pytest.skip("the drive serves an image from the SD card; these tests need the "
                    "generated disk (remove the card)")


@pytest.fixture(scope="module")
def raw_disk(drive):
    """The drive's raw device (/dev/rdiskN, \\\\.\\PhysicalDriveN) opened
    read-only: (Disk, fd). It isn't cached, so every byte read came from the drive."""
    from hddproto import gen_disk
    rig = make_host(lambda m: print(f"\n>>> {m}"))
    disk = rig.find_disk(gen_disk.DEFAULT_SECTORS * gen_disk.SECTOR_BYTES)
    fd = rig.open_raw(disk)
    yield disk, fd
    os.close(fd)


@pytest.fixture
def raw_disk_rw(drive):
    """The drive's raw device opened for reading and writing: (Disk, fd).
    Never on an SD image: the tests that write would corrupt it."""
    from hddproto import gen_disk
    require_generated_disk(drive)
    rig = make_host(lambda m: print(f"\n>>> {m}"))
    disk = rig.find_disk(gen_disk.DEFAULT_SECTORS * gen_disk.SECTOR_BYTES)
    fd = rig.open_raw(disk, write=True)
    yield disk, fd
    os.close(fd)


@pytest.fixture
def ask(request):
    """Shows a message to the person at the rig, even while pytest captures output."""
    capman = request.config.pluginmanager.getplugin("capturemanager")

    def show(message: str):
        with capman.global_and_fixture_disabled():
            print(f"\n>>> {message}", file=sys.stderr, flush=True)

    return show


@pytest.fixture
def rig(ask, drive):
    host = make_host(ask)
    host.drive = drive  # for HDDPRO_REPLUG=redetect
    return host


@pytest.fixture
def run_dir(request, session_stamp):
    path = RUNS / f"{session_stamp}-{request.node.name}"
    path.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    drive_mark = None
    if "drive" in request.fixturenames:
        drive_mark = request.getfixturevalue("drive").mark()
    yield path
    report = getattr(request.node, "report_call", None)
    if report is not None and report.failed:
        if drive_mark is not None:
            lines = request.getfixturevalue("drive").lines_since(drive_mark)
            (path / "drive.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
        host = make_host(lambda _m: None)
        minutes = int((time.monotonic() - started) // 60) + 1
        (path / "system.log").write_text(host.system_log(f"{minutes}m"), encoding="utf-8")
        (path / "disks.txt").write_text(host.disks(), encoding="utf-8")
        (path / "failure.txt").write_text(report.longreprtext, encoding="utf-8")
