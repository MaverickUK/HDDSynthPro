"""The slice 1 bus program (src/ide_bus.pio), co-simulated with an ATA PIO
mode 0 host that honours IORDY and a model of core 1. No hardware needed.

These pin down the timings the real bus will hold the firmware to:
  - IORDY is already low when the host samples it (tA = 35 ns after the strobe)
  - read data is on the bus before IORDY rises, and set up t5 = 50 ns
  - DD is released within t6z = 30 ns of DIOR# rising
  - IORDY never stays low longer than 1.25 us, given core 1's reply time
  - IORDY is never driven high, and the device never fights the host on DD
"""
from __future__ import annotations

import pytest

from hddproto.ide_host_model import Access, Core1, Mode0, run_host
from hddproto.pio_sim import CYCLE_PS, PioSim

pytestmark = pytest.mark.sim

MODE0 = Mode0()
DIOR_BIT = 22
PHASES_NS = [i * CYCLE_PS / 1000 / 6 for i in range(6)]  # start times across one PIO cycle


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("ide_bus")


def make_sim(program, bypass_dior=True) -> PioSim:
    """Configured as src/ide_bus.cpp configures the state machine."""
    return PioSim(program, out_base=0, out_count=16, sideset_base=24, jmp_pin=DIOR_BIT,
                  fifo_depth=4, tx_depth=4, bypass_mask=(1 << DIOR_BIT) if bypass_dior else 0)


def registers(values: dict):
    return lambda block, addr: values.get((block, addr), 0)


def check_clean(run):
    assert not run.errors, run.errors


def check_read(r, want):
    assert r.read_value == want, f"read {r.access}: got {r.read_value!r}, want {want:#06x}"
    assert r.iordy_low_at_tA, "IORDY was not low when the host sampled it"
    # Margin before the host samples IORDY (tA after the strobe): the chip
    # select is seen and IORDY pulled low with room to spare.
    assert r.iordy_low_lead_ns is not None and r.iordy_low_lead_ns + MODE0.tA >= 20, \
        f"IORDY low only {r.iordy_low_lead_ns + MODE0.tA:.0f} ns before the host samples it"
    assert r.wait_ns is not None and r.wait_ns < MODE0.iordy_max
    assert r.data_setup_ns is not None and r.data_setup_ns >= MODE0.t5
    assert r.release_ns is not None and r.release_ns <= MODE0.t6z, \
        f"DD released {r.release_ns} ns after DIOR# rose (limit {MODE0.t6z})"


@pytest.mark.parametrize("phase_ns", PHASES_NS)
def test_status_read(program, phase_ns):
    core1 = Core1(200, registers({("CMD", 7): 0x50}))
    run = run_host(make_sim(program), core1, [Access(False, "CMD", 7)], start_ns=100 + phase_ns)
    check_clean(run)
    check_read(run.results[0], 0x50)


@pytest.mark.parametrize("phase_ns", PHASES_NS)
def test_register_write(program, phase_ns):
    core1 = Core1(200, registers({}))
    run = run_host(make_sim(program), core1, [Access(True, "CMD", 6, 0x00A0)], start_ns=100 + phase_ns)
    check_clean(run)
    assert core1.writes == [("CMD", 6, 0x00A0)]
    r = run.results[0]
    assert r.iordy_low_at_tA  # IORDY is low at first; released as soon as the write is seen
    assert r.wait_ns is not None and r.wait_ns < 150


def test_identify_style_sequence(program):
    """Select, command, poll, then 256 data reads with CS0# held between them."""
    data = iter(range(0x1000, 0x1100))
    state = {"status_reads": 0}

    def read_value(block, addr):
        if (block, addr) == ("CMD", 0):
            return next(data)
        if (block, addr) == ("CTL", 6):
            return 0x58  # DRDY | DSC | DRQ
        if (block, addr) == ("CMD", 7):
            state["status_reads"] += 1
            return 0x58
        return 0

    core1 = Core1(250, read_value)
    accesses = [Access(True, "CMD", 6, 0xA0), Access(True, "CMD", 7, 0xEC),
                Access(False, "CTL", 6), Access(False, "CMD", 7)]
    accesses += [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)]
    run = run_host(make_sim(program), core1, accesses)
    check_clean(run)
    assert core1.writes == [("CMD", 6, 0xA0), ("CMD", 7, 0xEC)]
    reads = [r for r in run.results if not r.access.write]
    check_read(reads[0], 0x58)
    check_read(reads[1], 0x58)
    for i, r in enumerate(reads[2:]):
        check_read(r, 0x1000 + i)
    assert state["status_reads"] == 1
    assert run.rx_words == len(accesses)


def test_write_then_read_back(program):
    regs = {}

    class Echo(Core1):
        def on_word(self, t_ps, sample):
            super().on_word(t_ps, sample)
            if self.writes:
                block, addr, value = self.writes[-1]
                regs[(block, addr)] = value & 0xFF

    core1 = Echo(150, lambda block, addr: regs.get((block, addr), 0))
    accesses = [Access(True, "CMD", 2, 0x55), Access(True, "CMD", 3, 0xAA),
                Access(False, "CMD", 2), Access(False, "CMD", 3)]
    run = run_host(make_sim(program), core1, accesses)
    check_clean(run)
    check_read(run.results[2], 0x55)
    check_read(run.results[3], 0xAA)


def test_core1_reply_budget(program):
    """How long core 1 may take to answer a read before IORDY would be held
    past the 1.25 us limit. The PIO's own share must leave most of it."""
    run = run_host(make_sim(program), Core1(0, registers({("CMD", 7): 0x50})),
                   [Access(False, "CMD", 7)])
    check_clean(run)
    overhead_ns = run.results[0].wait_ns
    budget_ns = MODE0.iordy_max - overhead_ns
    print(f"\nPIO overhead {overhead_ns:.0f} ns: core 1 must answer a read within {budget_ns:.0f} ns")
    assert overhead_ns < 250

    # And a slow-but-legal core 1 still works.
    slow = Core1(budget_ns - 100, registers({("CMD", 7): 0x50}))
    run = run_host(make_sim(program), slow, [Access(False, "CMD", 7)])
    check_clean(run)
    check_read(run.results[0], 0x50)


def test_release_after_dior_rises(program):
    """DD is released within t6z of DIOR# rising, with or without the
    input-synchroniser bypass that ide_bus.cpp sets on DIOR#; the bypass
    roughly halves the time, which leaves margin for faster PIO modes."""
    worst = 0.0
    for phase in PHASES_NS:
        run = run_host(make_sim(program, bypass_dior=False), Core1(200, registers({("CMD", 7): 0x50})),
                       [Access(False, "CMD", 7)], start_ns=100 + phase)
        worst = max(worst, run.results[0].release_ns)
    with_bypass = max(
        run_host(make_sim(program), Core1(200, registers({("CMD", 7): 0x50})),
                 [Access(False, "CMD", 7)], start_ns=100 + p).results[0].release_ns
        for p in PHASES_NS)
    print(f"\nDD release after DIOR# rises: {with_bypass:.0f} ns with the bypass, {worst:.0f} ns without")
    assert with_bypass <= MODE0.t6z
    assert worst <= MODE0.t6z
    assert worst > with_bypass


def test_chip_select_without_strobe(program):
    """A chip select that comes and goes with no strobe: IORDY goes low while
    it is asserted, is released after, and nothing reaches core 1."""
    from hddproto.pio_sim import synced
    sim = make_sim(program)
    idle = (1 << 19) | (1 << 20) | (1 << 22) | (1 << 23) | (1 << 24) | (1 << 26)
    history, iordy = [], []
    for k in range(200):
        t_ns = k * CYCLE_PS / 1000
        pins = idle & ~(1 << 19) if 100 <= t_ns < 400 else idle
        history.append(pins)
        sim.step(synced(history, k, sim.bypass_mask))
        iordy.append((t_ns, bool((sim.pindirs >> 24) & 1)))
    low = [t for t, l in iordy if l]
    assert low and min(low) < 100 + MODE0.t1  # low within t1 of the chip select
    assert max(low) < 400 + 100                # released soon after it goes
    assert not sim.rx_fifo
