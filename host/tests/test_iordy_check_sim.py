"""Runs the real src/iordy_check.pio in the simulator: a read whose DIOR# rises
while IORDY is still low (a host that doesn't wait for IORDY) is reported,
with its register; a read the host waited for is not, even when ide_bus pulls
IORDY low again soon after for the next back-to-back data read; and DMA
strobes (IORDY high throughout) never are. No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

NS = 1000  # ps
DIOR = 1 << 22
IORDY_BIT = 24
IORDY = 1 << IORDY_BIT
CS0, CS1 = 1 << 19, 1 << 20
STATUS = 7 << 16  # DA2-0 = 7
IDLE = DIOR | IORDY | CS0 | CS1


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("iordy_check")


def make_sim(program) -> PioSim:
    """Configured as iordy_timer.cpp configures the state machine."""
    return PioSim(program, jmp_pin=IORDY_BIT, fifo_depth=4)


def reads(waves):
    """A waveform from a list of (dior_fall_ns, dior_rise_ns, iordy_low_from_ns,
    iordy_high_at_ns, iordy_low_again_ns or None), all for a command-block
    status read (CS0# low)."""
    def wave(t_ps: int) -> int:
        t = t_ps / NS
        pins = IDLE
        for fall, rise, low_from, high_at, low_again in waves:
            if fall - 70 <= t < rise + 20:
                pins &= ~CS0
                pins |= STATUS
            if fall <= t < rise:
                pins &= ~DIOR
            if low_from <= t < high_at or (low_again is not None and low_again <= t < low_again + 300):
                pins &= ~IORDY
        return pins
    return wave


def run(program, waves, duration_ns=3000):
    return pio_sim.run(make_sim(program), reads(waves), int(duration_ns * NS))


def test_host_that_waits_is_not_reported(program):
    # IORDY held 600 ns, DIOR# rises 10 ns after IORDY does (the host's delay).
    assert run(program, [(200, 810, 130, 800, None)]) == []


def test_host_that_ignores_iordy_is_reported(program):
    # PIO mode 0's 165 ns strobe, regardless of IORDY held to 800 ns.
    words = run(program, [(200, 365, 130, 800, None)])
    assert len(words) == 1
    assert (words[0] >> 16) & 7 == 7  # the register: STATUS
    assert not (words[0] & CS0)       # command block


@pytest.mark.parametrize("delay_ns", [5, 10, 30])
@pytest.mark.parametrize("again_ns", [20, 25, 40])
def test_back_to_back_reads_are_not_reported(program, delay_ns, again_ns):
    """Data reads with CS held: ide_bus pulls IORDY low again for the next read
    about 20 ns after DIOR# rises. The check is done before that."""
    rise = 800 + delay_ns
    assert run(program, [(200, rise, 130, 800, rise + again_ns)]) == []


def test_dma_strobes_are_not_reported(program):
    def wave(t_ps: int) -> int:
        t = t_ps / NS
        pins = IDLE  # no chip select, IORDY high
        if (t % 120) < 70 and 100 <= t < 2500:
            pins &= ~DIOR
        return pins
    assert pio_sim.run(make_sim(program), wave, 3000 * NS) == []


def test_several_ignored_reads_each_reported(program):
    waves = [(200 + i * 600, 365 + i * 600, 130 + i * 600, 500 + i * 600, None) for i in range(3)]
    assert len(run(program, waves, 2500)) == 3
