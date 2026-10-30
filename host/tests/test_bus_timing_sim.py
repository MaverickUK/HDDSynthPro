"""Runs the real src/bus_timing.pio in the simulator: each read's t1 (chip
select to DIOR# falling) and strobe (DIOR# low) come out as bus_timing.cpp
converts them, within one pass of each loop. No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

CYCLE_NS = pio_sim.CYCLE_PS / 1000
DIOR_BIT = 22
CS0, CS1, DIOR, DIOW = 1 << 19, 1 << 20, 1 << 22, 1 << 23
IDLE = CS0 | CS1 | DIOR | DIOW


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("bus_timing")


def measure(program, accesses, duration_ns):
    """accesses: (cs_ns, dior_fall_ns or None for a write, dior_rise_ns, cs_end_ns)."""
    sim = PioSim(program, jmp_pin=DIOR_BIT, fifo_depth=4, in_shift_left=True)
    history = []
    for k in range(int(duration_ns / CYCLE_NS)):
        t = k * CYCLE_NS
        pins = IDLE
        for cs, fall, rise, end in accesses:
            if cs <= t < end:
                pins &= ~CS0
                if fall is not None and fall <= t < rise:
                    pins &= ~DIOR
                elif fall is None and cs + 50 <= t < rise:
                    pins &= ~DIOW
        history.append(pins)
        sim.step(pio_sim.synced(history, k, 0))
    # As bus_timing.cpp: t1 passes of 6 cycles, strobe passes of 2.
    return [((w & 0xFFFF) * 6 * CYCLE_NS + 20, (w >> 16) * 2 * CYCLE_NS + 35) for w in sim.rx_fifo]  # + T1_LATE_NS, STROBE_LATE_NS


@pytest.mark.parametrize("t1", [0, 30, 70, 120, 200])
@pytest.mark.parametrize("strobe", [100, 165, 290, 400])
def test_read_timing(program, t1, strobe):
    (got_t1, got_strobe), = measure(program, [(100, 100 + t1, 100 + t1 + strobe, 100 + t1 + strobe + 20)], 1500)
    assert abs(got_t1 - t1) <= 20, (got_t1, t1)
    assert abs(got_strobe - strobe) <= 20, (got_strobe, strobe)


def test_writes_are_not_timed(program):
    """A write (DIOW#, not DIOR#) ends with its chip select: no report, and
    the read after it is timed from its own chip select."""
    got = measure(program, [(100, None, 400, 450), (700, 770, 935, 960)], 1500)
    assert len(got) == 1 and abs(got[0][0] - 70) <= 35
