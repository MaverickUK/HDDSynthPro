"""The Evesham's wrong data word (10 Nov 2026, docs/atapi.md): one read of a
CD sector carried the word before it again, with `iordy_ignored=0`. These
hold the bus programs to what that analysis rests on, in the simulator:

  - back-to-back data reads with CS held: ide_bus pulls IORDY low again soon
    enough for the next strobe even with recovery (t2i) far below PIO mode 3's
    70 ns, so a late IORDY can't be how the host came not to wait;
  - a data read the host doesn't wait for, and so reads wrong, is always
    flagged by iordy_check, running beside ide_bus on the same pins: whatever
    core 1's latency, the strobe's length or the recovery before it, and with
    the undriven DD lines keeping the last word (as the cable does) and IORDY
    rising slowly through the host's pull-up.

So a wrong word with nothing flagged points away from the host skipping its
wait: see docs/atapi.md for what's left. (With strobes 10 ns apart the
synchronised bus observer can merge two reads into one word; iordy_check
still flags the unwaited one.)
"""
from __future__ import annotations

import dataclasses

import pytest

from hddproto.ide_host_model import FAST_PIO_MODES, Access, Core1, run_host
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

DIOR_BIT, IORDY_BIT = 22, 24
MODE3 = FAST_PIO_MODES[3]
WORDS = [0x1000 + 0x111 * i for i in range(8)]


@pytest.fixture(scope="module")
def programs(assemble):
    return assemble("ide_bus"), assemble("iordy_check"), assemble("bus_observer", "bus_observer")


def make_sims(programs):
    """ide_bus as src/ide_bus.cpp configures it (DIOR# unsynchronised); the two
    observers as iordy_timer.cpp and bus_observer.cpp do (synchronised)."""
    bus, check, observer = programs
    sim = PioSim(bus, out_base=0, out_count=16, sideset_base=IORDY_BIT, jmp_pin=DIOR_BIT, fifo_depth=4,
                 tx_depth=4, bypass_mask=1 << DIOR_BIT, status_rx_below=4, autopush=True)
    sim.pc = bus.offsets["idle"]
    return sim, PioSim(check, jmp_pin=IORDY_BIT, fifo_depth=8), PioSim(observer, jmp_pin=DIOR_BIT, fifo_depth=64)


def data_reads(core1_ns, timing, ignore=None, **kw):
    sim, check, observer = make_sims(kw.pop("programs"))
    words = iter(WORDS)
    core1 = Core1(core1_ns, lambda block, addr: next(words) if (block, addr) == ("CMD", 0) else 0x58)
    accesses = [Access(False, "CMD", 0, hold_cs=i < len(WORDS) - 1, ignore_iordy=i == ignore)
                for i in range(len(WORDS))]
    run = run_host(sim, core1, accesses, timing=timing, observers=(check, observer), dd_keeper=True, **kw)
    return run, check, observer


@pytest.mark.parametrize("t2i_ns", [70, 40, 20])
@pytest.mark.parametrize("core1_ns", [60, 200])
def test_iordy_is_low_in_time_for_back_to_back_reads(programs, t2i_ns, core1_ns):
    run, check, _ = data_reads(core1_ns, dataclasses.replace(MODE3, t2i=t2i_ns), programs=programs)
    assert [r.read_value for r in run.results] == WORDS
    assert all(r.iordy_low_at_tA for r in run.results)
    assert check.rx_fifo == []  # the host waited for every read


@pytest.mark.parametrize("rise_ns", [0, 25])
@pytest.mark.parametrize("t2i_ns", [10, 70])
@pytest.mark.parametrize("t2_ns", [60, 100, 200])
@pytest.mark.parametrize("core1_ns", [40, 100, 300])
def test_an_unwaited_read_that_comes_back_wrong_is_flagged(programs, core1_ns, t2_ns, t2i_ns, rise_ns):
    bad = 5
    timing = dataclasses.replace(MODE3, t2=t2_ns, t2i=t2i_ns)
    run, check, observer = data_reads(core1_ns, timing, ignore=bad, programs=programs, iordy_rise_ns=rise_ns)
    got = [r.read_value for r in run.results]
    if got[bad] != WORDS[bad]:
        assert check.rx_fifo, f"read {bad} came back {got[bad]:#06x} unflagged"
        if t2i_ns >= 70:  # (with 10 ns between strobes the synchronised observer can merge two reads)
            assert observer.rx_fifo[bad] & 0xFFFF == got[bad]  # it saw what the host did
    else:
        assert got == WORDS
