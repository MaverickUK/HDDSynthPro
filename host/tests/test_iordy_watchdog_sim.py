"""Runs the real src/iordy_watchdog.pio in the simulator (`isa = on`): when
ide_bus has held IORDY low for about 23 us with no strobe, it releases it,
and only then. ide_bus is modelled as the other writer of IORDY's pin
direction (the two share the PIO block's register), the host's pull-up as a
slow rise, and the host's strobes as DIOR# low. Set up as ide_bus.cpp sets
it up: the count in ISR, DIOR# past the input synchroniser. No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

IORDY_BIT = 24  # GPIO 28 as a sample bit
DIOR_BIT, DIOW_BIT = 22, 23
PASSES = 200_000_000 * 23 // 1_000_000 // 6  # as ide_bus.cpp works it out at 200 MHz
CYCLE_NS = pio_sim.CYCLE_PS / 1000
RISE_NS = 500   # released IORDY reads high this long after (the host's pull-up and the cable)


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("iordy_watchdog")


def run(program, pulls: list[tuple[float, float]], duration_ns: float,
        strobes: list[tuple[float, float]] = ()) -> list[float]:
    """ide_bus pulls IORDY low over each (from_ns, to_ns) window (writing its
    pin direction at both ends, as `arm` and `idle` do); the host holds DIOR#
    low over each of `strobes`. Returns when the watchdog released IORDY, ns."""
    sim = PioSim(program, jmp_pin=IORDY_BIT, set_base=IORDY_BIT, set_count=1, bypass_mask=1 << DIOR_BIT,
                 isr=PASSES - 1)
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
        dior = 0 if any(start <= t < end for start, end in strobes) else 1
        history.append(level << IORDY_BIT | dior << DIOR_BIT | 1 << DIOW_BIT)
        before = pulled
        sim.step(pio_sim.synced(history, k, sim.bypass_mask))
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


def test_leaves_back_to_back_reads_alone(program):
    """The Evesham (11 Nov 2026): data reads 225-465 ns long, back to back, with
    IORDY released at each one's end and pulled low again 25 ns after DIOR#
    rises, too soon for the host's pull-up to raise it at the pin. The old
    watchdog, which started over only on IORDY high, took 23 us of them for
    one hold and released IORDY mid-read; the strobes start the count over."""
    pulls, strobes = [], []
    t = 1000.0
    while t < 60000:
        strobe = 225 if len(strobes) % 7 == 3 else 400
        strobes.append((t, t + strobe))
        pulls.append((t - 50, t + strobe - 60))  # low from the chip select until the data is driven
        t += strobe + 120  # recovery
    assert run(program, pulls, 62000, strobes) == []


def test_a_stuck_chip_select_after_reads_is_still_released(program):
    """Reads, then the chip select left asserted with no strobe: released 23 us
    after the last strobe."""
    strobes = [(1000 + 600 * i, 1400 + 600 * i) for i in range(10)]
    last = strobes[-1][1]
    pulls = [(s - 50, e - 60) for s, e in strobes[:-1]] + [(strobes[-1][0] - 50, 10**9)]
    releases = run(program, pulls, last + 30000, strobes)
    assert releases, "never released"
    assert 20000 < releases[0] - last < 26000, f"released {releases[0] - last:.0f} ns after the last strobe"
