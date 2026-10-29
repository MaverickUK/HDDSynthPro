"""Multiword DMA's data phase (src/ide_dma.pio), co-simulated with a multiword
DMA host (hddproto.mdma_host_model) in modes 0, 1 and 2. No hardware needed.

These pin down the timings the bus will hold the programs to:
  - read data is on DD within tE of DIOR# falling (150, 60, 50 ns) and held
    tF = 5 ns after it rises, and DD is driven only while DIOR# is low
  - write data is taken as DIOW# rises, though it's valid only tG (100, 30,
    20 ns) before that
  - DMARQ falls as each burst's last strobe begins, within tL (35-120 ns),
    never before, so the host neither stops early nor strobes once too often
  - bursts follow one another: IRQ 0 after each, and the next one starts
"""
from __future__ import annotations

import pytest

from hddproto.mdma_host_model import B_DMARQ, MDMA_MODES, run_mdma
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

BYPASS = (1 << 22) | (1 << 23)  # DIOR# and DIOW# skip the synchroniser, as the C side sets
MODES = list(MDMA_MODES)


@pytest.fixture(scope="module")
def read_program(assemble):
    return assemble("ide_dma", "ide_dma_read")


@pytest.fixture(scope="module")
def write_program(assemble):
    return assemble("ide_dma", "ide_dma_write")


def make_sim(program, write: bool) -> PioSim:
    """Configured as the C side configures the state machine: TX joined for
    reads (the words come that way), 4 each way for writes."""
    return PioSim(program, out_base=0, out_count=16, sideset_base=B_DMARQ, bypass_mask=BYPASS,
                  fifo_depth=4, tx_depth=4 if write else 8)


def sectors(n: int, seed: int = 0) -> list[list[int]]:
    return [[(i * 7919 + s * 131 + seed) & 0xFFFF for i in range(256)] for s in range(n)]


def timeout_for(timing, words: int) -> float:
    return words * timing.t0 * 1.5 + 20_000


@pytest.mark.parametrize("mode", MODES)
def test_read_sectors(read_program, mode):
    t = MDMA_MODES[mode]
    bursts = sectors(2)
    run = run_mdma(make_sim(read_program, write=False), t, bursts, write=False,
                   timeout_ns=timeout_for(t, 512))
    assert not run.errors, run.errors
    assert [s.value for s in run.strobes] == [w for b in bursts for w in b]
    assert run.bursts_done == 2
    for s in run.strobes:
        assert s.valid_from_ns is not None and s.valid_from_ns <= s.fall_ns + t.tE, \
            f"data valid {s.valid_from_ns - s.fall_ns:.0f} ns after DIOR# fell (tE {t.tE})"
        assert s.held_ns is not None and s.held_ns >= t.tF, f"data held {s.held_ns} ns (tF {t.tF})"


@pytest.mark.parametrize("mode", MODES)
def test_write_sectors(write_program, mode):
    t = MDMA_MODES[mode]
    bursts = sectors(2, seed=5)
    run = run_mdma(make_sim(write_program, write=True), t, bursts, write=True,
                   timeout_ns=timeout_for(t, 512))
    assert not run.errors, run.errors
    assert run.received == [w for b in bursts for w in b]
    assert len(run.strobes) == 512
    assert run.bursts_done == 2


@pytest.mark.parametrize("write", [False, True])
@pytest.mark.parametrize("mode", MODES)
def test_dmarq_falls_on_each_bursts_last_strobe(read_program, write_program, mode, write):
    """DMARQ falls after the last strobe of a burst begins and within tL; the
    host then stops, so each burst is exactly its words long."""
    t = MDMA_MODES[mode]
    bursts = [[1, 2, 3], [4], [5, 6, 7, 8, 9]]  # short and single-word bursts too
    program = write_program if write else read_program
    run = run_mdma(make_sim(program, write), t, bursts, write=write, timeout_ns=timeout_for(t, 9))
    assert not run.errors, run.errors
    assert len(run.strobes) == 9
    limit = t.tLw if write else t.tLr
    lasts = [s for s in run.strobes if s.dmarq_low_after_fall_ns is not None]
    assert len(lasts) == 3  # once per burst, on its last strobe
    ends = [2, 3, 8]  # each burst's last strobe
    assert [run.strobes.index(s) for s in lasts] == ends
    for s in lasts:
        assert 0 < s.dmarq_low_after_fall_ns <= limit, \
            f"DMARQ fell {s.dmarq_low_after_fall_ns:.0f} ns after the strobe (tL {limit})"
    got = [s.value for s in run.strobes] if not write else run.received
    assert got == [w for b in bursts for w in b]


@pytest.mark.parametrize("delay_ns", [0, 300, 2000])
def test_dmarq_waits_for_the_first_word(read_program, delay_ns):
    """Core 1 puts the count in the FIFO, then starts DMA, so the first word
    comes a little later. DMARQ must not rise before it: the host's first
    strobe would find nothing on DD (0.16.1 on the rig: word 0 of every
    sector read 0048h)."""
    t = MDMA_MODES[2]
    bursts = sectors(2)
    run = run_mdma(make_sim(read_program, write=False), t, bursts, write=False,
                   timeout_ns=timeout_for(t, 512), dma_delay_ns=delay_ns)
    assert not run.errors, run.errors
    assert [s.value for s in run.strobes] == [w for b in bursts for w in b]
    for s in run.strobes:
        assert s.valid_from_ns is not None and s.valid_from_ns <= s.fall_ns + t.tE


@pytest.mark.parametrize("mode", MODES)
def test_dd_driven_only_during_read_strobes(read_program, mode):
    """Between strobes, and once a burst is over, DD is released, so a host
    that stops early never finds the bus driven."""
    t = MDMA_MODES[mode]
    sim = make_sim(read_program, write=False)
    run = run_mdma(sim, t, sectors(1), write=False, timeout_ns=timeout_for(t, 256))
    assert not run.errors, run.errors
    assert sim.pindirs & 0xFFFF == 0  # released at the end
    for s in run.strobes:
        assert s.held_ns is not None and s.held_ns < 25, f"DD driven {s.held_ns} ns after DIOR# rose"
