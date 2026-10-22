"""Runs the real src/ide_listen.pio (slice 0) in the simulator against ATA
PIO mode 0 waveforms. No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim, trace
from hddproto.pio_sim import CYCLE_PS, PioSim

pytestmark = pytest.mark.sim

NS = 1000  # ps

# Sample layout (see src/bus_sample.h)
DIOR = 1 << 22
DIOW = 1 << 23
CS0 = 1 << 19
CS1 = 1 << 20
IDLE = DIOR | DIOW | CS0 | CS1 | (1 << 24) | (1 << 26)  # strobes, CS#, IORDY, RESET# high

# ATA PIO mode 0 timings (ns)
T0_CYCLE = 600
T1_ADDR_SETUP = 70
T2_PULSE = 165
T3_WRITE_SETUP = 60
T4_WRITE_HOLD = 30
T9_ADDR_HOLD = 20


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("ide_listen")


def access(t0_ns: float, write: bool, cs: int, addr: int, data: int, junk: int = 0xDEAD):
    """One mode 0 register access starting at t0_ns, as a function of time.
    Lines are only valid inside their spec windows; outside them they carry junk."""
    strobe = DIOW if write else DIOR
    s_fall = t0_ns + T1_ADDR_SETUP
    s_rise = s_fall + T2_PULSE

    def wave(t_ps: int) -> int | None:
        t = t_ps / NS
        if not (t0_ns <= t < s_rise + T9_ADDR_HOLD):
            return None
        pins = IDLE & ~cs
        pins |= (addr & 7) << 16
        if s_fall <= t < s_rise:
            pins &= ~strobe
        if write:
            valid = s_rise - T3_WRITE_SETUP <= t < s_rise + T4_WRITE_HOLD
            pins |= data if valid else junk
        else:
            pins |= junk  # nobody drives in slice 0
        return pins

    return wave


def combine(*waves):
    def wave(t_ps):
        for w in waves:
            v = w(t_ps)
            if v is not None:
                return v
        return IDLE | 0xBEEF
    return wave


@pytest.mark.parametrize("phase_ps", range(0, CYCLE_PS, 1111))
def test_single_write_captures_valid_data(program, phase_ps):
    w = access(100 + phase_ps / NS, write=True, cs=CS0, addr=6, data=0x00A0)
    words = pio_sim.run(PioSim(program), combine(w), 1_000 * NS)
    assert len(words) == 1
    s = words[0]
    assert trace.op_of(s) == "WR"
    assert s & 0xFFFF == 0x00A0
    assert (s >> 16) & 7 == 6
    assert not s & CS0


@pytest.mark.parametrize("phase_ps", range(0, CYCLE_PS, 1111))
def test_single_read_is_one_word(program, phase_ps):
    w = access(100 + phase_ps / NS, write=False, cs=CS1, addr=6, data=0)
    words = pio_sim.run(PioSim(program), combine(w), 1_000 * NS)
    assert len(words) == 1
    assert trace.op_of(words[0]) == "RD"
    assert not words[0] & CS1


def test_back_to_back_cycles_in_order(program):
    waves = [access(100 + i * T0_CYCLE, write=True, cs=CS0, addr=i % 8, data=i) for i in range(200)]
    words = pio_sim.run(PioSim(program), combine(*waves), (200 * T0_CYCLE + 1000) * NS)
    assert [w & 0xFFFF for w in words] == list(range(200))
    assert [(w >> 16) & 7 for w in words] == [i % 8 for i in range(200)]


def test_both_strobes_low_is_recorded(program):
    def wave(t_ps):
        t = t_ps / NS
        if 100 <= t < 300:
            return IDLE & ~DIOR & ~DIOW & ~CS0
        return IDLE
    words = pio_sim.run(PioSim(program), wave, 1_000 * NS)
    assert len(words) == 1
    assert trace.op_of(words[0]) == "BOTH"


def test_full_fifo_stalls(program):
    waves = [access(100 + i * T0_CYCLE, write=True, cs=CS0, addr=0, data=i) for i in range(12)]
    sim = PioSim(program)
    pio_sim.run(sim, combine(*waves), (12 * T0_CYCLE + 1000) * NS, drain_every_cycles=None)
    assert len(sim.rx_fifo) == 8
    assert sim.rx_stall
