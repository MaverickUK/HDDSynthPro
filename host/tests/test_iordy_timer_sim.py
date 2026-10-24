"""Runs the real src/iordy_timer.pio in the simulator: a read that IORDY holds
longer than the threshold is reported with its length; a quick one is not.
No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim
from hddproto.pio_sim import CYCLE_PS, PioSim

pytestmark = pytest.mark.sim

NS = 1000  # ps

# Sample layout (see src/bus_sample.h)
DIOR = 1 << 22
IORDY_BIT = 24
IORDY = 1 << IORDY_BIT

THRESHOLD_ITERATIONS = 30  # 2 cycles each: 400 ns at 150 MHz, as iordy_timer.cpp sets it


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("iordy_timer")


def read_held(start_ns: float, hold_ns: float):
    """One read: DIOR# falls at start_ns; IORDY (held low from before the
    strobe, as ide_bus does from chip select) rises hold_ns later; DIOR#
    rises 150 ns after that."""
    def wave(t_ps: int) -> int:
        t = t_ps / NS
        pins = DIOR | IORDY
        if start_ns - 100 <= t < start_ns + hold_ns:
            pins &= ~IORDY
        if start_ns <= t < start_ns + hold_ns + 150:
            pins &= ~DIOR
        return pins
    return wave


def held_ns(word: int) -> float:
    """What iordy_timer.cpp makes of a report (its iterationsToNs)."""
    iterations = THRESHOLD_ITERATIONS + word
    return (2 * iterations + 2) * CYCLE_PS / NS


def run_one(program, hold_ns: float) -> list[int]:
    sim = PioSim(program, jmp_pin=IORDY_BIT, fifo_depth=4, tx_fifo=[THRESHOLD_ITERATIONS])
    return pio_sim.run(sim, read_held(200, hold_ns), int((hold_ns + 1000) * NS))


@pytest.mark.parametrize("hold_ns", [50, 200, 350])
def test_quick_read_is_not_reported(program, hold_ns):
    assert run_one(program, hold_ns) == []


@pytest.mark.parametrize("hold_ns", [500, 1000, 7200])
def test_slow_read_is_timed(program, hold_ns):
    words = run_one(program, hold_ns)
    assert len(words) == 1
    # Within the input synchroniser and loop granularity: a few cycles.
    assert held_ns(words[0]) == pytest.approx(hold_ns, abs=30)


def test_each_slow_read_reported_once(program):
    def wave(t_ps: int) -> int:
        t = t_ps / NS
        return read_held(200, 900)(t_ps) if t < 1500 else read_held(2000, 600)(t_ps)
    sim = PioSim(program, jmp_pin=IORDY_BIT, fifo_depth=4, tx_fifo=[THRESHOLD_ITERATIONS])
    words = pio_sim.run(sim, wave, 4000 * NS)
    assert [round(held_ns(w), -2) for w in words] == [900, 600]
