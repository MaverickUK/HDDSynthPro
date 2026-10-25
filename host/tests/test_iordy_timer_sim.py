"""Runs the real src/iordy_timer.pio in the simulator: a read that IORDY holds
longer than the threshold is reported with its length and its register; a
quick one is not; one that starts while IORDY is still high is reported as
early. No hardware needed."""
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
CS1 = 1 << 20      # CS1# high: a command-block access (with CS0# low)
STATUS_ADDR = 7

THRESHOLD_ITERATIONS = 30  # 2 cycles each: 400 ns at 150 MHz, as iordy_timer.cpp sets it


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("iordy_timer")


def read_held(start_ns: float, hold_ns: float, iordy_low_from_ns: float = -100):
    """One read: DIOR# falls at start_ns; IORDY (held low from before the
    strobe, as ide_bus does from chip select, unless `iordy_low_from_ns` says
    when after the strobe it falls) rises hold_ns later; DIOR# rises 150 ns
    after that."""
    def wave(t_ps: int) -> int:
        t = t_ps / NS
        pins = DIOR | IORDY | CS1 | (STATUS_ADDR << 16)
        if start_ns + iordy_low_from_ns <= t < start_ns + hold_ns:
            pins &= ~IORDY
        if start_ns <= t < start_ns + hold_ns + 150:
            pins &= ~DIOR
        return pins
    return wave


def held_ns(word: int) -> float:
    """What iordy_timer.cpp makes of a report (its iterationsToNs)."""
    iterations = THRESHOLD_ITERATIONS + (0xFFFF - (word >> 16))
    return (2 * iterations + 4) * CYCLE_PS / NS


def register_bits(word: int) -> int:
    """DA2-0, CS0#, CS1#: bits 16-20 of the sample, in the report's low bits."""
    return word & 0x1F


def make_sim(program) -> PioSim:
    """Configured as src/iordy_timer.cpp configures the state machine."""
    return PioSim(program, jmp_pin=IORDY_BIT, fifo_depth=4, tx_fifo=[THRESHOLD_ITERATIONS])


def run_one(program, hold_ns: float) -> list[int]:
    sim = make_sim(program)
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
    assert register_bits(words[0]) == STATUS_ADDR | 0x10  # STATUS: DA 7, CS0# low, CS1# high


def test_each_slow_read_reported_once(program):
    def wave(t_ps: int) -> int:
        t = t_ps / NS
        return read_held(200, 900)(t_ps) if t < 1500 else read_held(2000, 600)(t_ps)
    words = pio_sim.run(make_sim(program), wave, 4000 * NS)
    assert [round(held_ns(w), -2) for w in words] == [900, 600]


def is_early(word: int) -> bool:
    """iordy_timer.cpp's test: the IORDY bit of the sample, report bit 8."""
    return bool(word & (1 << (IORDY_BIT - 16)))


@pytest.mark.parametrize("hold_ns", [200, 1000])
def test_timed_reads_are_not_early(program, hold_ns):
    assert not any(is_early(w) for w in run_one(program, hold_ns))


@pytest.mark.parametrize("low_after_ns", [10, 30])
def test_read_starting_with_iordy_high_is_early(program, low_after_ns):
    """IORDY falls only after the strobe: within tA the host may still wait,
    but there's no margin. Reported once, untimed, with its register."""
    words = pio_sim.run(make_sim(program), read_held(200, 600, iordy_low_from_ns=low_after_ns), 2000 * NS)
    assert len(words) == 1 and is_early(words[0])
    assert register_bits(words[0]) == STATUS_ADDR | 0x10
