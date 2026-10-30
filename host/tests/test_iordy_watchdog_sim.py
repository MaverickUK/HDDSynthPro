"""Runs the real src/iordy_watchdog.pio in the simulator (`isa = on`): when
ide_bus has held IORDY low for about 23 us, it releases it, and only then.
ide_bus is modelled as the other writer of IORDY's pin direction (the two
share the PIO block's register), and the host's pull-up as a slow rise.
No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

IORDY_BIT = 24  # GPIO 28 as a sample bit
CYCLE_NS = pio_sim.CYCLE_PS / 1000
RISE_NS = 500   # released IORDY reads high this long after (the host's pull-up and the cable)


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("iordy_watchdog")


def run(program, pulls: list[tuple[float, float]], duration_ns: float) -> list[float]:
    """ide_bus pulls IORDY low over each (from_ns, to_ns) window (writing its
    pin direction at both ends, as `arm` and `idle` do). Returns when the
    watchdog released it, ns."""
    sim = PioSim(program, jmp_pin=IORDY_BIT, set_base=IORDY_BIT, set_count=1)
    history, releases = [], []
    high_since = 0.0
    for k in range(int(duration_ns / CYCLE_NS)):
        t = k * CYCLE_NS
        for start, end in pulls:  # ide_bus's writes
            if start <= t < start + CYCLE_NS:
                sim.pindirs |= 1 << IORDY_BIT
            if end <= t < end + CYCLE_NS:
                sim.pindirs &= ~(1 << IORDY_BIT)
        pulled = bool((sim.pindirs >> IORDY_BIT) & 1)
        if pulled:
            high_since = None
        elif high_since is None:
            high_since = t
        level = 0 if pulled or t - high_since < RISE_NS else 1
        history.append(level << IORDY_BIT)
        before = pulled
        sim.step(pio_sim.synced(history, k, 0))
        if before and not (sim.pindirs >> IORDY_BIT) & 1 and not any(e <= t < e + CYCLE_NS for _, e in pulls):
            releases.append(t)
    return releases


def test_releases_a_stuck_chip_select(program):
    """The freeze: IORDY held from 1 us with no strobe to end it."""
    releases = run(program, [(1000, 10**9)], 60000)
    assert releases, "never released"
    assert 20000 < releases[0] - 1000 < 26000, f"released after {releases[0] - 1000:.0f} ns"


def test_leaves_real_accesses_alone(program):
    """IORDY low for 1 us at a time, every 3 us (reads with core 1 slow), and a
    hold of 15 us (the ICL's longest status-to-data gap, 11 us, and more): the
    watchdog never steps in."""
    pulls = [(1000 + 3000 * i, 2000 + 3000 * i) for i in range(10)] + [(40000, 55000)]
    assert run(program, pulls, 60000) == []


def test_starts_over_after_each_release(program):
    """A hold that ends just before the limit, then another from scratch:
    the time doesn't carry over."""
    pulls = [(1000, 19000), (20000, 38000)]
    assert run(program, pulls, 45000) == []
