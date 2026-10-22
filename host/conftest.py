"""Shared fixtures for the rig tests.

  drive    the drive's serial log (opened once per session, waits for READY)
  rig      the host side: replug the adapter, system log (macOS for now)
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
from hddproto.host_macos import MacHost
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
    return lambda name: pio_sim.assemble(REPO / "src" / f"{name}.pio", pioasm)


@pytest.fixture(scope="session")
def session_stamp():
    return datetime.now().strftime("%Y%m%d-%H%M%S")


@pytest.fixture(scope="session")
def drive(session_stamp):
    RUNS.mkdir(parents=True, exist_ok=True)
    log = DriveLog(raw_log_path=RUNS / f"{session_stamp}-serial.log")
    log.open()
    log.command("h", "READY", timeout=5)
    yield log
    log.close()


@pytest.fixture
def ask(request):
    """Shows a message to the person at the rig, even while pytest captures output."""
    capman = request.config.pluginmanager.getplugin("capturemanager")

    def show(message: str):
        with capman.global_and_fixture_disabled():
            print(f"\n>>> {message}", file=sys.stderr, flush=True)

    return show


@pytest.fixture
def rig(ask):
    return MacHost(ask)


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
            (path / "drive.log").write_text("\n".join(lines) + "\n")
        host = MacHost(lambda _m: None)
        minutes = int((time.monotonic() - started) // 60) + 1
        (path / "system.log").write_text(host.system_log(f"{minutes}m"))
        (path / "disks.txt").write_text(host.disks())
        (path / "failure.txt").write_text(report.longreprtext)
