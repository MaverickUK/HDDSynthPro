"""The slice 1 bus program (src/ide_bus.pio), co-simulated with an ATA PIO
host that honours IORDY and a model of core 1, in PIO modes 0 to 3 (the
modes IDENTIFY can offer). No hardware needed.

These pin down the timings the real bus will hold the firmware to, in each mode:
  - IORDY is already low when the host samples it (tA = 35 ns after the
    strobe), though the chip selects come only t1 = 70 to 30 ns earlier
  - read data is on the bus before IORDY rises, and set up t5 = 50 to 20 ns
  - DD is released within t6z = 30 ns of DIOR# rising
  - IORDY never stays low longer than 1.25 us, given core 1's reply time
  - IORDY is never driven high, and the device never fights the host on DD
"""
from __future__ import annotations

import pytest

from hddproto.ide_host_model import FAST_PIO_MODES, PIO_MODES, Access, Core1, IsaBus, Mode0, run_host
from hddproto.pio_sim import CYCLE_PS, PioSim

pytestmark = pytest.mark.sim

MODE0 = Mode0()
PIO_MODES = {**PIO_MODES, 3: FAST_PIO_MODES[3]}  # modes 0-3, the ones IDENTIFY can offer
MODES = list(PIO_MODES)  # parametrize over these
# How long before the host samples IORDY (tA after the strobe) it must be low:
# room for the cable and, on the PCB, the bus switches (under a nanosecond).
# Mode 3's chip selects come 30 ns before the strobe, which leaves 24. (Mode 4,
# not offered: 25 ns, leaving 19; and its write data, valid only 20 ns before
# DIOW# rises, can fall between two of the program's samples.)
IORDY_MARGIN_NS = 15
DIOR_BIT = 22
PHASES_NS = [i * CYCLE_PS / 1000 / 6 for i in range(6)]  # start times across one PIO cycle


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("ide_bus")


def make_sim(program, bypass_dior=True) -> PioSim:
    """Configured as src/ide_bus.cpp configures the state machine: autopush,
    and starting at `idle`."""
    sim = PioSim(program, out_base=0, out_count=16, sideset_base=24, jmp_pin=DIOR_BIT,
                 fifo_depth=4, tx_depth=4, bypass_mask=(1 << DIOR_BIT) if bypass_dior else 0,
                 status_rx_below=4, autopush=True)
    sim.pc = program.offsets["idle"]
    return sim


def registers(values: dict):
    return lambda block, addr: values.get((block, addr), 0)


def check_clean(run):
    assert not run.errors, run.errors


def check_read(r, want, timing: Mode0 = MODE0):
    assert r.read_value == want, f"read {r.access}: got {r.read_value!r}, want {want:#06x}"
    assert r.iordy_low_at_tA, "IORDY was not low when the host sampled it"
    # Margin before the host samples IORDY (tA after the strobe): the chip
    # select is seen and IORDY pulled low with room to spare.
    assert r.iordy_low_lead_ns is not None and r.iordy_low_lead_ns + timing.tA >= IORDY_MARGIN_NS, \
        f"IORDY low only {r.iordy_low_lead_ns + timing.tA:.0f} ns before the host samples it"
    assert r.wait_ns is not None and r.wait_ns < timing.iordy_max
    assert r.data_setup_ns is not None and r.data_setup_ns >= timing.t5
    assert r.release_ns is not None and r.release_ns <= timing.t6z, \
        f"DD released {r.release_ns} ns after DIOR# rose (limit {timing.t6z})"


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("phase_ns", PHASES_NS)
def test_status_read(program, phase_ns, mode):
    timing = PIO_MODES[mode]
    core1 = Core1(200, registers({("CMD", 7): 0x50}))
    run = run_host(make_sim(program), core1, [Access(False, "CMD", 7)], timing=timing,
                   start_ns=100 + phase_ns)
    check_clean(run)
    check_read(run.results[0], 0x50, timing)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("addr", [2, 3, 4, 5, 7])
def test_foreign_control_reads_not_driven(program, addr, mode):
    """An ISA card's CS1# covers 3F0-3F7, where the floppy controller answers
    3F2-3F5 (and bit 7 of 3F7): the drive must leave DD alone for those reads,
    only answering Alternate Status (CTL 6), and still release IORDY."""
    timing = PIO_MODES[mode]
    core1 = Core1(200, registers({("CTL", addr): 0x1234, ("CTL", 6): 0x50}))
    run = run_host(make_sim(program), core1, [Access(False, "CTL", addr), Access(False, "CTL", 6)],
                   timing=timing)
    foreign, alt = run.results
    assert foreign.read_value is None, f"DD driven for CTL {addr}: {foreign.read_value:#06x}"
    assert foreign.wait_ns is not None and foreign.wait_ns < timing.iordy_max
    check_read(alt, 0x50, timing)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("phase_ns", PHASES_NS)
def test_register_write(program, phase_ns, mode):
    core1 = Core1(200, registers({}))
    run = run_host(make_sim(program), core1, [Access(True, "CMD", 6, 0x00A0)], timing=PIO_MODES[mode],
                   start_ns=100 + phase_ns)
    check_clean(run)
    assert core1.writes == [("CMD", 6, 0x00A0)]
    r = run.results[0]
    assert r.iordy_low_at_tA  # IORDY is low at first; released as soon as the write is seen
    assert r.wait_ns is not None and r.wait_ns < 150


@pytest.mark.parametrize("mode", MODES)
def test_identify_style_sequence(program, mode):
    """Select, command, poll, then 256 data reads with CS0# held between them."""
    timing = PIO_MODES[mode]
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
    run = run_host(make_sim(program), core1, accesses, timing=timing)
    check_clean(run)
    assert core1.writes == [("CMD", 6, 0xA0), ("CMD", 7, 0xEC)]
    reads = [r for r in run.results if not r.access.write]
    check_read(reads[0], 0x58, timing)
    check_read(reads[1], 0x58, timing)
    for i, r in enumerate(reads[2:]):
        check_read(r, 0x1000 + i, timing)
    assert state["status_reads"] == 1
    assert run.rx_words == len(accesses)


@pytest.mark.parametrize("mode", MODES)
def test_write_then_read_back(program, mode):
    timing = PIO_MODES[mode]
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
    run = run_host(make_sim(program), core1, accesses, timing=timing)
    check_clean(run)
    check_read(run.results[2], 0x55, timing)
    check_read(run.results[3], 0xAA, timing)


@pytest.mark.parametrize("mode", MODES)
def test_sector_write_with_a_slow_core1(program, mode):
    # WRITE SECTORS: 256 data words back to back, while core 1 takes 1 us per
    # access. Writes don't wait for an answer, so without flow control the
    # FIFO fills and words are lost; IORDY must hold the host instead.
    words = [(i * 7919) & 0xFFFF for i in range(256)]
    core1 = Core1(200, registers({}), service_ns=1000)
    accesses = [Access(True, "CMD", 0, w, hold_cs=True) for w in words]
    run = run_host(make_sim(program), core1, accesses, timing=PIO_MODES[mode], tail_ns=10000)
    check_clean(run)
    assert [v for (_, _, v) in core1.writes] == words
    waits = [r.wait_ns for r in run.results if r.wait_ns is not None]
    assert waits, "the host never had to wait: the test isn't exercising flow control"
    assert max(waits) < MODE0.iordy_max


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


@pytest.mark.parametrize("mode", MODES)
def test_release_after_dior_rises(program, mode):
    """DD is released within t6z of DIOR# rising, with or without the
    input-synchroniser bypass that ide_bus.cpp sets on DIOR#; the bypass
    roughly halves the time, which leaves margin for faster PIO modes."""
    worst = 0.0
    for phase in PHASES_NS:
        run = run_host(make_sim(program, bypass_dior=False), Core1(200, registers({("CMD", 7): 0x50})),
                       [Access(False, "CMD", 7)], timing=PIO_MODES[mode], start_ns=100 + phase)
        worst = max(worst, run.results[0].release_ns)
    with_bypass = max(
        run_host(make_sim(program), Core1(200, registers({("CMD", 7): 0x50})),
                 [Access(False, "CMD", 7)], timing=PIO_MODES[mode], start_ns=100 + p).results[0].release_ns
        for p in PHASES_NS)
    print(f"\nDD release after DIOR# rises: {with_bypass:.0f} ns with the bypass, {worst:.0f} ns without")
    assert with_bypass <= MODE0.t6z
    assert worst <= MODE0.t6z
    assert worst > with_bypass


@pytest.mark.parametrize("mode", MODES)
def test_chip_select_without_strobe(program, mode):
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
    # Low before the host samples it (tA after a strobe that comes t1 after
    # the chip select), with the margin check_read asks for: in mode 0 and 1
    # that's before the strobe, in mode 2 (t1 = 30 ns) just after it.
    t = PIO_MODES[mode]
    assert low and min(low) < 100 + t.t1 + t.tA - IORDY_MARGIN_NS
    assert max(low) < 400 + 100                # released soon after it goes
    assert not sim.rx_fifo


# ---- ISA hosts ----------------------------------------------------------------
#
# The same program serves ISA cards (iordy_watchdog.pio, test_iordy_watchdog_sim.py,
# deals with a chip select that brings no strobe).

ISA = IsaBus()
IDLE_PINS = (1 << 19) | (1 << 20) | (1 << 22) | (1 << 23) | (1 << 24) | (1 << 26)
ALIASED = IDLE_PINS & ~(1 << 19) & ~(7 << 16)  # CS0# low, DA2-0 = 0 (1F0h), no strobe


@pytest.mark.parametrize("phase_ns", PHASES_NS)
def test_isa_status_read(program, phase_ns):
    core1 = Core1(200, registers({("CMD", 7): 0x50}))
    run = run_host(make_sim(program), core1, [Access(False, "CMD", 7)], timing=ISA, start_ns=100 + phase_ns)
    check_clean(run)
    check_read(run.results[0], 0x50, ISA)


def test_isa_identify_burst(program):
    data = iter(range(0x1000, 0x1100))
    core1 = Core1(250, lambda b, a: next(data) if (b, a) == ("CMD", 0) else 0x58)
    accesses = [Access(True, "CMD", 6, 0xA0), Access(True, "CMD", 7, 0xEC), Access(False, "CMD", 7)]
    accesses += [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)]
    run = run_host(make_sim(program), core1, accesses, timing=ISA)
    check_clean(run)
    assert core1.writes == [("CMD", 6, 0xA0), ("CMD", 7, 0xEC)]
    reads = [r for r in run.results if not r.access.write]
    check_read(reads[0], 0x58, ISA)
    for i, r in enumerate(reads[1:]):
        check_read(r, 0x1000 + i, ISA)


def test_isa_sector_write_with_a_slow_core1(program):
    """Flow control holds the host when the RX FIFO is full."""
    words = [(i * 7919) & 0xFFFF for i in range(256)]
    core1 = Core1(200, registers({}), service_ns=1000)
    accesses = [Access(True, "CMD", 0, w, hold_cs=True) for w in words]
    run = run_host(make_sim(program), core1, accesses, timing=ISA, tail_ns=10000)
    check_clean(run)
    assert [v for (_, _, v) in core1.writes] == words
    waits = [r.wait_ns for r in run.results if r.wait_ns is not None]
    assert waits and max(waits) < ISA.iordy_max


def test_chip_select_without_strobe_holds_iordy(program):
    """The program on its own holds IORDY for as long as a chip select lasts
    (the watchdog, with `isa = on`, is what lets go), and nothing reaches core 1."""
    from hddproto.pio_sim import synced
    sim = make_sim(program)
    history, low = [], []
    for k in range(int(10000 * 1000 / CYCLE_PS)):
        t = k * CYCLE_PS / 1000
        history.append(ALIASED if t >= 100 else IDLE_PINS)
        sim.step(synced(history, k, sim.bypass_mask))
        low.append(bool((sim.pindirs >> 24) & 1))
    assert low.index(True) * CYCLE_PS / 1000 <= 100 + 45
    assert all(low[int(200 * 1000 / CYCLE_PS):])
    assert not sim.rx_fifo


def test_write_sample_reads_as_a_write(program):
    """A write's sample is taken as DIOW# rises, with DIOW# forced to 0, so
    core 1 (bus::op) sees the write it was, with its register and data."""
    core1 = Core1(200, registers({}))
    run = run_host(make_sim(program), core1, [Access(True, "CMD", 3, 0x1234)], timing=MODE0)
    check_clean(run)
    assert core1.writes == [("CMD", 3, 0x1234)]
