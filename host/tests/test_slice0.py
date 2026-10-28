"""Slice 0: listen only. Replug the adapter and check what the drive saw.

With no drive answering, an adapter probes forever (the JMicron on the
Windows rig: about 420,000 accesses a second, and a RESET# pulse every 4.8 s),
so the bus never goes quiet and the trace ring would overflow. The test turns
on the listen build's capture window (console `w`): each RESET# assert opens
a window of cfg::LISTEN_WINDOW_EVENTS accesses, and the rest are counted
(`skipped=` in `s`) but not recorded until the next RESET#. Once a window has
been printed the trace goes quiet until the next pulse, and what it holds is
complete.

Only the part from the first RESET# release is judged: whatever the lines do
while the adapter's power comes and goes is in the report, but isn't the probe.

Exit criterion: a clean, repeatable trace of the adapter's probe sequence.
`--save-reference` saves it as host/reference/slice0_probe_<adapter>.txt
($HDDPRO_ADAPTER, from host/rig.env: adapters probe differently); later runs
compare their opening sequence with it.
"""
from __future__ import annotations

import os
import shutil
from pathlib import Path

import pytest

from conftest import require_build
from hddproto import trace
from hddproto.paths import REFERENCE

pytestmark = pytest.mark.slice0


@pytest.fixture(autouse=True)
def listen_build(firmware_slice):
    require_build(firmware_slice, drive_build=False)


FIRST_RESET_TIMEOUT_S = 90  # covers a manual replug
PROBE_START_TIMEOUT_S = 10  # the first access comes about 2.6 s after the RESET# release
QUIET_S = 2                 # the windows have closed once the trace is quiet this long
QUIET_TIMEOUT_S = 30
UNPLUGGED_QUIET_S = 8       # longer than the adapter's RESET# pulses with no drive (every 4.8 s)
UNPLUG_TIMEOUT_S = 120
OPENING_STEPS = 40          # how much of the probe must match the reference


def reference_path(suffix: str = "") -> Path:
    adapter = os.environ.get("HDDPRO_ADAPTER", "").strip()
    return REFERENCE / f"slice0_probe{'_' + adapter if adapter else ''}{suffix}.txt"


def set_window(drive, on: bool):
    if f"window={int(on)}" not in drive.command("s", "STATS"):
        drive.command("w", "WINDOW")


@pytest.fixture
def window(drive):
    """The capture window on for the test, and off again afterwards."""
    set_window(drive, True)
    yield
    set_window(drive, False)


@pytest.fixture
def probe(drive, rig, ask, run_dir, request, window) -> list[str]:
    """Replugs the adapter and returns the trace lines from the first RESET# release."""
    if not rig.automatic_replug:
        # By hand: unplugged first, until the bus is quiet. Plugged in with
        # no drive answering, the adapter pulses RESET# every few seconds,
        # which would be taken for the power-up probe.
        ask("Unplug the USB-IDE adapter's USB cable and leave it out until asked "
            "(the BB48 stays powered).")
        try:
            drive.wait_quiet(UNPLUGGED_QUIET_S, timeout=UNPLUG_TIMEOUT_S)
        except TimeoutError:
            pytest.fail("the adapter kept resetting the bus: was it unplugged?")
    before = drive.command("s", "STATS")
    mark = drive.mark()
    if rig.automatic_replug:
        rig.replug()
    else:
        ask("Now plug the adapter's USB cable back in.")
    drive.wait_for(lambda l: l.startswith("RESET") and l.endswith("RELEASE"), since=mark,
                   timeout=FIRST_RESET_TIMEOUT_S)
    # After power-up the adapter resets twice, then waits about 2.6 s before
    # its first command-block access: wait for that, not for quiet, since the
    # pause is longer than QUIET_S and its next RESET# pulse comes too soon
    # for a longer one.
    try:
        drive.wait_for(lambda l: l.startswith("EV") and " CMD " in l, since=mark, timeout=PROBE_START_TIMEOUT_S)
    except TimeoutError:
        pass  # judged below: no accesses
    quiet = True
    try:
        drive.wait_quiet(QUIET_S, timeout=QUIET_TIMEOUT_S)
    except TimeoutError:
        quiet = False  # resets keep coming: report what was captured
    lines = drive.lines_since(mark)
    after = drive.command("s", "STATS")

    first_release = next(i for i, l in enumerate(lines) if l.startswith("RESET") and l.endswith("RELEASE"))
    before_release, probe_lines = lines[:first_release], lines[first_release:]
    (run_dir / "trace.txt").write_text("\n".join(lines) + "\n")
    report = "\n".join([
        f"{len(before_release)} lines before the first RESET# release (power coming up), not judged:",
        *("  " + l for l in before_release[-20:]),
        "From the first release:",
        trace.summary(trace.parse(probe_lines)),
        "Opening sequence:",
        *("  " + s for s in trace.opening_sequence(probe_lines, OPENING_STEPS)),
        f"Quiet afterwards: {quiet}",
        f"Before: {before}",
        f"After:  {after}",
    ]) + "\n"
    (run_dir / "summary.txt").write_text(report)
    print(report)
    return probe_lines


def test_probe_sequence(probe: list[str], request, run_dir):
    t = trace.parse(probe)
    assert t.accesses, "no bus activity after the RESET# release"
    assert not t.stalls, "the PIO FIFO filled: accesses were missed"
    assert not t.dropped, "the trace ring overflowed inside a window"
    assert not [a for a in t.accesses if a.op == "BOTH"], "DIOR# and DIOW# low together"
    touched = {(a.op, a.reg) for a in t.accesses if a.block == "CMD"}
    assert ("RD", "STATUS") in touched or ("WR", "DEVICE") in touched, \
        f"the adapter never read STATUS or wrote DEVICE (saw {sorted(touched)})"

    # Repeatable: the same opening as the saved reference.
    reference = reference_path()
    if request.config.getoption("--save-reference"):
        # Only a probe that passed the checks above becomes the reference.
        REFERENCE.mkdir(exist_ok=True)
        reference.write_text("\n".join(probe) + "\n")
        shutil.copy(run_dir / "summary.txt", reference_path("_summary"))
        return  # this run is the reference now
    if not reference.exists():
        pytest.skip("clean, but no reference to compare with: run once with --save-reference")
    want = trace.opening_sequence(reference.read_text().splitlines(), OPENING_STEPS)
    got = trace.opening_sequence(probe, OPENING_STEPS)
    rows = [f"  {'  ' if w == g else '!='} {w:28} {g}"
            for w, g in zip(want + [""] * (len(got) - len(want)), got + [""] * (len(want) - len(got)))]
    assert got == want, "the opening differs from the reference (reference, then this run):\n" + "\n".join(rows)
