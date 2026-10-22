"""Slice 0: listen only. Replug the adapter and check what the drive saw.

Exit criterion: a clean, repeatable trace of the adapter's probe sequence,
saved as host/reference/slice0_probe.txt (run with --save-reference).
"""
from __future__ import annotations

import shutil

import pytest

from hddproto import trace
from hddproto.paths import REFERENCE

pytestmark = pytest.mark.slice0

FIRST_ACTIVITY_TIMEOUT_S = 90  # covers a manual replug
QUIET_S = 3                    # the probe is over once the bus is quiet this long
PROBE_TIMEOUT_S = 120


@pytest.fixture
def probe(drive, rig, run_dir, request) -> trace.Trace:
    """Replugs the adapter and returns the trace of everything it did."""
    drive.command("s", "STATS")
    mark = drive.mark()
    rig.replug()
    drive.wait_for(lambda l: l.startswith(("EV", "RESET")), since=mark,
                   timeout=FIRST_ACTIVITY_TIMEOUT_S)
    drive.wait_quiet(QUIET_S, timeout=PROBE_TIMEOUT_S)
    lines = drive.lines_since(mark)
    stats = drive.command("s", "STATS")

    t = trace.parse(lines)
    (run_dir / "trace.txt").write_text("\n".join(lines) + "\n")
    report = trace.summary(t) + f"\n{stats}\n"
    (run_dir / "summary.txt").write_text(report)
    print(report)
    if request.config.getoption("--save-reference"):
        REFERENCE.mkdir(exist_ok=True)
        shutil.copy(run_dir / "trace.txt", REFERENCE / "slice0_probe.txt")
        shutil.copy(run_dir / "summary.txt", REFERENCE / "slice0_probe_summary.txt")
    return t


def test_probe_sequence(probe: trace.Trace):
    assert not probe.empty, "no bus activity after the replug"
    assert not probe.stalls and not probe.dropped, "the trace lost accesses"
    assert not [a for a in probe.accesses if a.op == "BOTH"], "DIOR# and DIOW# low together"
    hard = any(kind == "ASSERT" for _, kind in probe.resets)
    assert hard or probe.soft_resets(), "no hardware (RESET#) or soft (SRST) reset"
    touched = {(a.op, a.reg) for a in probe.accesses if a.block == "CMD"}
    assert ("RD", "STATUS") in touched or ("WR", "DEVICE") in touched, \
        f"the adapter never read STATUS or wrote DEVICE (saw {sorted(touched)})"
