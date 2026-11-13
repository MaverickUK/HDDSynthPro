"""Runs the real src/bus_observer.pio in the simulator: each read gives one
word, the bus as it was just before DIOR# rose, with a sequence number that
counts down. No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

NS = 1000  # ps

# Sample layout (see src/bus_sample.h)
DIOR_BIT = 22
DIOR = 1 << DIOR_BIT
CS1 = 1 << 20      # CS1# high: a command-block access (with CS0# low)
SEQ_SHIFT = 21     # as src/sector_check.h


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("bus_observer", "bus_observer")


def make_sim(program) -> PioSim:
    """Configured as src/bus_observer.cpp configures the state machine."""
    return PioSim(program, jmp_pin=DIOR_BIT, fifo_depth=8)


def reads(*cycles):
    """Reads of the data register. Each is (start_ns, length_ns, first, last):
    DD holds `first`, then `last` for the final 60 ns of the strobe."""
    def wave(t_ps: int) -> int:
        t = t_ps / NS
        pins = DIOR | CS1
        for start, length, first, last in cycles:
            if start <= t < start + length:
                pins &= ~DIOR
                pins |= last if t >= start + length - 60 else first
            elif start + length <= t < start + length + 15:
                pins |= last  # held a moment after DIOR# rises, then released
        return pins
    return wave


def test_one_word_per_read_with_the_final_value(program):
    words = pio_sim.run(make_sim(program), reads((100, 400, 0x1111, 0xBEEF), (700, 300, 0x0000, 0x1234)),
                        1300 * NS)
    assert [w & 0xFFFF for w in words] == [0xBEEF, 0x1234]
    assert all((w >> 16) & 0x1F == CS1 >> 16 for w in words)  # data register: DA 0, CS0# low


def test_sequence_counts_down(program):
    cycles = [(100 + i * 400, 250, 0, i) for i in range(5)]
    words = pio_sim.run(make_sim(program), reads(*cycles), 2300 * NS)
    seqs = [w >> SEQ_SHIFT for w in words]
    assert len(seqs) == 5
    assert all((a - 1) & 0x7FF == b for a, b in zip(seqs, seqs[1:]))


# ---- bus_observer_strobe (HDDPRO_OBSERVER_STROBE) ------------------------------
# As src/sector_check.h lays its words out: the passes in bits 21-26, counting
# down from 63, one per 15 ns; the sequence number in bits 27-31.
STROBE_SHIFT, STROBE_MASK, STROBE_PASS_NS = 21, 0x3FF, 15  # 10 bits of passes (13 Nov 2026: 960 ns was too short)
STROBE_SEQ_SHIFT, STROBE_SEQ_MASK = 31, 0x1


@pytest.fixture(scope="module")
def strobe_program(assemble):
    return assemble("bus_observer", "bus_observer_strobe")


def strobe_ns(word: int) -> int:
    return ((STROBE_MASK - ((word >> STROBE_SHIFT) & STROBE_MASK)) & STROBE_MASK) * STROBE_PASS_NS


def test_strobe_variant_gives_the_same_data(strobe_program):
    words = pio_sim.run(make_sim(strobe_program), reads((100, 400, 0x1111, 0xBEEF), (700, 300, 0x0000, 0x1234)),
                        1300 * NS)
    assert [w & 0xFFFF for w in words] == [0xBEEF, 0x1234]
    assert all((w >> 16) & 0x1F == CS1 >> 16 for w in words)


@pytest.mark.parametrize("length_ns", [60, 90, 120, 200, 300, 450, 900])
def test_strobe_variant_measures_how_long_dior_was_low(strobe_program, length_ns):
    """To within a pass or two (15 ns each, and the synchroniser's lag at
    both ends cancels): enough to tell a read the host cut short (under about
    100 ns) from one it waited for (hundreds)."""
    words = pio_sim.run(make_sim(strobe_program), reads((100, length_ns, 0, 0x55AA)), (length_ns + 400) * NS)
    assert len(words) == 1
    assert abs(strobe_ns(words[0]) - length_ns) <= 2 * STROBE_PASS_NS


def test_strobe_variant_counts_past_a_microsecond(strobe_program):
    # The Innostor bridge's Status reads last about 1.6 us (its slow IORDY):
    # the count wraps only after 1,024 passes (15.4 us).
    words = pio_sim.run(make_sim(strobe_program), reads((100, 1600, 0, 1)), 2200 * NS)
    assert abs(strobe_ns(words[0]) - 1600) <= 2 * STROBE_PASS_NS


def test_strobe_variant_sequence_counts_down(strobe_program):
    cycles = [(100 + i * 400, 250, 0, i) for i in range(40)]  # past a wrap of the 1-bit count
    words = pio_sim.run(make_sim(strobe_program), reads(*cycles), 16200 * NS)
    seqs = [(w >> STROBE_SEQ_SHIFT) & STROBE_SEQ_MASK for w in words]
    assert len(seqs) == 40
    assert all((a - 1) & STROBE_SEQ_MASK == b for a, b in zip(seqs, seqs[1:]))
