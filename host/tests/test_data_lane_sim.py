"""The data lane (src/ide_bus.pio's ide_bus_data and ide_data_in, one PIO
block, docs/data-lane.md), co-simulated with the ATA PIO host, a model of
core 1 and a model of the DMA channel that feeds ide_data_in. No hardware.

Data register reads must never reach core 1: ide_data_in answers them from
its FIFO, which the DMA channel keeps filled. Everything else is served as
ide_bus serves it (test_pio_bus_sim.py), and must still meet the same
timings: IORDY low at tA, read data set up t5 before DIOR# rises, DD released
within t6z, IORDY held less than 1.25 us. And core 1 must be able to tell
when the host has read a whole block (the channel idle and the FIFO empty:
ide_data_in has taken the spare word), and not before.
"""
from __future__ import annotations

import pytest

from hddproto.ide_host_model import FAST_PIO_MODES, PIO_MODES, Access, Core1, IsaBus, Mode0, run_host
from hddproto.pio_sim import CYCLE_PS, DmaToTx, PioBlock, PioSim, synced

pytestmark = pytest.mark.sim

MODES = {**PIO_MODES, 3: FAST_PIO_MODES[3]}  # modes 0-3, the ones IDENTIFY can offer
ISA = IsaBus()
DIOR_BIT = 22
IORDY_BIT = 24
DATA_ADDRESS = 16  # DA2-0 = 0, CS0# low, CS1# high: what ide_bus.cpp puts in Y
SPARE = 0xDEAD     # the word the DMA channel adds after each block
IORDY_MARGIN_NS = 15
PHASES_NS = [i * CYCLE_PS / 1000 / 6 for i in range(6)]
STATUS_READY, STATUS_DRQ = 0x50, 0x58


@pytest.fixture(scope="module")
def programs(assemble):
    return assemble("ide_bus", "ide_bus_data"), assemble("ide_bus", "ide_data_in")


def make_block(programs) -> PioBlock:
    """Configured as src/ide_bus.cpp configures the two state machines."""
    bus_program, data_program = programs
    bus = PioSim(bus_program, out_base=0, out_count=16, sideset_base=IORDY_BIT, jmp_pin=DIOR_BIT,
                 fifo_depth=4, tx_depth=4, bypass_mask=1 << DIOR_BIT, status_rx_below=4, autopush=True)
    bus.pc = bus_program.offsets["idle"]
    bus.y = DATA_ADDRESS
    data = PioSim(data_program, out_base=0, out_count=16, fifo_depth=4, tx_depth=8,  # TX joined
                  bypass_mask=1 << DIOR_BIT)
    return PioBlock(bus, data)


class LaneCore1(Core1):
    """Core 1 with the data lane, as ide_bus.cpp runs it: when a data-in block
    begins it restarts ide_data_in (empty FIFO, IRQ 4 clear) and starts the
    channel with the block and the spare word; the block is done when the
    channel is idle and the FIFO is empty, and then the next one (if any)
    starts. STATUS reads DRQ while a block is under way. Data reads must
    never reach it."""

    def __init__(self, latency_ns: float, blocks: list[list[int]], start_on_command: bool = True,
                 registers: dict | None = None, service_ns: float = 0):
        self.regs = dict(registers or {})
        super().__init__(latency_ns, self._value, service_ns)
        self.blocks = [list(b) for b in blocks]
        self.dma: DmaToTx | None = None
        self.started = 0
        self.done_ns: list[float] = []
        self.queued_ns: list[float] = []  # when each block's last word (the spare) was queued
        self.start_on_command = start_on_command
        self.data_reads_seen = 0

    def _value(self, block, addr):
        if (block, addr) == ("CMD", 0):
            self.data_reads_seen += 1
            return 0xBAD0
        if (block, addr) in (("CMD", 7), ("CTL", 6)):
            return STATUS_DRQ if self.dma is not None else STATUS_READY
        return self.regs.get((block, addr), 0)

    def on_word(self, t_ps, sample):
        super().on_word(t_ps, sample)
        if self.writes and self.writes[-1][:2] == ("CMD", 7) and self.start_on_command:
            self.start_on_command = False
            self._start_pending = True

    _start_pending = False

    def start_block(self, block: PioBlock):
        data_sm = block.sms[1]
        data_sm.tx_fifo.clear()
        data_sm.pc = 0
        data_sm.osr = 0
        data_sm._delay = 0
        block.irq_flags &= ~(1 << 4)
        self.dma = DmaToTx(data_sm, self.blocks.pop(0) + [SPARE])
        block.on_cycle = self.dma
        self.started += 1

    def deliver(self, t_ps, block: PioBlock):
        if self.dma is not None and not self.dma.busy and len(self.queued_ns) < self.started:
            self.queued_ns.append(t_ps / 1000)
        if self.dma is not None and not self.dma.busy and not block.sms[1].tx_fifo:
            self.done_ns.append(t_ps / 1000)
            self.dma = None
            block.on_cycle = None
            if self.blocks:
                self._start_pending = True
        if self._start_pending and self.dma is None and self.blocks:
            self._start_pending = False
            self.start_block(block)
        super().deliver(t_ps, block)


def check_read(r, want, timing: Mode0, lane: bool = False):
    """A read's value and timing. A register read (core 1's answer) must find
    IORDY low when the host samples it; a data read from the lane may have
    its word on DD and IORDY released by then (on an ISA bus, tA = 125 ns),
    which is fine: then only the data's own timing matters."""
    assert r.read_value == want, f"read {r.access}: got {r.read_value!r}, want {want:#06x}"
    if not lane:
        assert r.iordy_low_at_tA, "IORDY was not low when the host sampled it"
        assert r.iordy_low_lead_ns is not None and r.iordy_low_lead_ns + timing.tA >= IORDY_MARGIN_NS
    if r.iordy_low_at_tA:
        assert r.wait_ns is not None and r.wait_ns < timing.iordy_max
    assert r.data_setup_ns is not None and r.data_setup_ns >= timing.t5, \
        f"data set up only {r.data_setup_ns} ns before DIOR# rose (t5 {timing.t5})"
    assert r.release_ns is not None and r.release_ns <= timing.t6z, \
        f"DD released {r.release_ns} ns after DIOR# rose (limit {timing.t6z})"


def words_for(n: int, seed: int = 1) -> list[int]:
    return [(i * 7919 + seed * 104729) & 0xFFFF for i in range(n)]


def identify_accesses():
    return ([Access(True, "CMD", 6, 0xA0), Access(True, "CMD", 7, 0xEC),
             Access(False, "CTL", 6), Access(False, "CMD", 7)]
            + [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)])


# ---- registers: as ide_bus ------------------------------------------------------

@pytest.mark.parametrize("mode", list(MODES))
@pytest.mark.parametrize("phase_ns", PHASES_NS)
def test_status_read(programs, phase_ns, mode):
    timing = MODES[mode]
    core1 = LaneCore1(200, [])
    run = run_host(make_block(programs), core1, [Access(False, "CMD", 7)], timing=timing,
                   start_ns=100 + phase_ns)
    assert not run.errors, run.errors
    check_read(run.results[0], STATUS_READY, timing)


@pytest.mark.parametrize("mode", list(MODES))
@pytest.mark.parametrize("phase_ns", PHASES_NS)
def test_register_write(programs, phase_ns, mode):
    core1 = LaneCore1(200, [], start_on_command=False)
    run = run_host(make_block(programs), core1, [Access(True, "CMD", 6, 0x00A0)], timing=MODES[mode],
                   start_ns=100 + phase_ns)
    assert not run.errors, run.errors
    assert core1.writes == [("CMD", 6, 0x00A0)]
    assert run.results[0].iordy_low_at_tA


@pytest.mark.parametrize("mode", list(MODES))
@pytest.mark.parametrize("addr", [2, 3, 4, 5, 7])
def test_foreign_control_reads_not_driven(programs, addr, mode):
    timing = MODES[mode]
    core1 = LaneCore1(200, [], registers={("CTL", addr): 0x1234})
    run = run_host(make_block(programs), core1, [Access(False, "CTL", addr), Access(False, "CTL", 6)],
                   timing=timing)
    foreign, alt = run.results
    assert foreign.read_value is None, f"DD driven for CTL {addr}"
    assert foreign.wait_ns is not None and foreign.wait_ns < timing.iordy_max
    check_read(alt, STATUS_READY, timing)


# ---- data reads: from the lane --------------------------------------------------

@pytest.mark.parametrize("timing", [*MODES.values(), ISA], ids=[*(f"mode{m}" for m in MODES), "isa"])
def test_identify(programs, timing):
    """Select, IDENTIFY, two status polls, 256 data reads with CS0# held:
    every word from the lane, none reaching core 1, and the block seen as
    done only after the last word's strobe."""
    words = words_for(256)
    core1 = LaneCore1(250, [words])
    accesses = identify_accesses()
    run = run_host(make_block(programs), core1, accesses, timing=timing)
    assert not run.errors, run.errors
    assert core1.writes == [("CMD", 6, 0xA0), ("CMD", 7, 0xEC)]
    assert core1.data_reads_seen == 0, "a data read reached core 1"
    assert run.rx_words == 4, "only the four register accesses reach core 1"
    reads = [r for r in run.results if not r.access.write]
    check_read(reads[0], STATUS_DRQ, timing)
    check_read(reads[1], STATUS_DRQ, timing)
    for i, r in enumerate(reads[2:]):
        check_read(r, words[i], timing, lane=True)
    assert len(core1.done_ns) == 1
    assert core1.done_ns[0] >= reads[-1].rise_ns, "block seen as done before the host finished it"
    assert core1.done_ns[0] - reads[-1].rise_ns < 100, "block seen as done late"
    # Every word is queued (the channel idle) while the host still has several
    # to read: the time core 1 uses to move the device on (ide_bus.cpp, step 2).
    unread = sum(1 for r in reads[2:] if r.fall_ns > core1.queued_ns[0])
    print(f"\n{timing.__class__.__name__}: channel done with {unread} words still to read")
    assert unread >= 8


@pytest.mark.parametrize("mode", list(MODES))
def test_sectors_back_to_back(programs, mode):
    """READ SECTORS, three sectors: a status poll before each, then its 256
    words. Core 1 starts each block once the last is done (the read-ahead
    already in), so the next status poll sees DRQ again."""
    timing = MODES[mode]
    sectors = [words_for(256, s) for s in range(3)]
    core1 = LaneCore1(250, sectors)
    accesses = [Access(True, "CMD", 7, 0x20)]
    for _ in sectors:
        accesses.append(Access(False, "CMD", 7))
        accesses += [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)]
    run = run_host(make_block(programs), core1, accesses, timing=timing, tail_ns=2000)
    assert not run.errors, run.errors
    assert core1.data_reads_seen == 0
    reads = [r for r in run.results if not r.access.write]
    for s, sector in enumerate(sectors):
        status, data = reads[s * 257], reads[s * 257 + 1: (s + 1) * 257]
        check_read(status, STATUS_DRQ, timing)
        for i, r in enumerate(data):
            check_read(r, sector[i], timing, lane=True)
    assert len(core1.done_ns) == 3


@pytest.mark.parametrize("mode", list(MODES))
def test_status_in_the_middle_of_a_block(programs, mode):
    """An Alternate Status read between data words goes to core 1, and takes
    no word from the lane: the next data read gets the next word."""
    timing = MODES[mode]
    words = words_for(256, 7)
    core1 = LaneCore1(250, [words])
    accesses = [Access(True, "CMD", 7, 0x20)]
    accesses += [Access(False, "CMD", 0, hold_cs=True) for _ in range(100)]
    accesses.append(Access(False, "CTL", 6))
    accesses += [Access(False, "CMD", 0, hold_cs=i < 155) for i in range(156)]
    run = run_host(make_block(programs), core1, accesses, timing=timing)
    assert not run.errors, run.errors
    data = [r for r in run.results if r.access.addr == 0 and r.access.block == "CMD"]
    alt = [r for r in run.results if r.access.block == "CTL"][0]
    check_read(alt, STATUS_DRQ, timing)
    for i, r in enumerate(data):
        check_read(r, words[i], timing, lane=True)
    assert core1.data_reads_seen == 0


@pytest.mark.parametrize("mode", list(MODES))
def test_status_then_data_with_cs_held(programs, mode):
    """CS0# stays low from a STATUS read to the data reads, only DA2-0
    changing: the program reads the address at each strobe, so the data
    reads still go to the lane."""
    timing = MODES[mode]
    words = words_for(256, 3)
    core1 = LaneCore1(250, [words])
    accesses = [Access(True, "CMD", 7, 0x20), Access(False, "CMD", 7, hold_cs=True)]
    accesses += [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)]
    run = run_host(make_block(programs), core1, accesses, timing=timing)
    assert not run.errors, run.errors
    reads = [r for r in run.results if not r.access.write]
    check_read(reads[0], STATUS_DRQ, timing)
    for i, r in enumerate(reads[1:]):
        check_read(r, words[i], timing, lane=True)


def test_host_that_ignores_iordy(programs):
    """A host that ends every strobe after t2 whatever IORDY says (a PIIX in
    compatible timing, an ISA card without IORDY; docs/no-iordy.md): the
    lane's words are on DD in time, where core 1's answers never could be.
    Modes 0-2 only: ATA makes IORDY compulsory from mode 3 up, and there the
    80 ns strobe is too short for a word found at the strobe."""
    from dataclasses import replace
    for mode in (0, 1, 2):
        t = replace(MODES[mode], waits_for_iordy=False)
        words = words_for(256, mode)
        core1 = LaneCore1(250, [words])
        accesses = [Access(True, "CMD", 7, 0xEC)]
        accesses += [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)]
        run = run_host(make_block(programs), core1, accesses, timing=t)
        assert not run.errors, (mode, run.errors)
        data = [r for r in run.results if not r.access.write]
        assert [r.read_value for r in data] == words, f"mode {mode}"
        setup = min(r.data_setup_ns for r in data)
        print(f"\nmode {mode}, host ignoring IORDY: data set up at least {setup:.0f} ns (t5 {t.t5:.0f})")
        assert setup >= t.t5, f"mode {mode}"


# ---- writes and odd cases -------------------------------------------------------

@pytest.mark.parametrize("mode", list(MODES))
def test_data_writes_reach_core1(programs, mode):
    """WRITE SECTORS' data: sampled for core 1 as ide_bus samples it, with
    IORDY holding the host while the RX FIFO is full; the lane isn't touched."""
    words = words_for(256, 5)
    core1 = LaneCore1(200, [], start_on_command=False, service_ns=1000)
    block = make_block(programs)
    accesses = [Access(True, "CMD", 0, w, hold_cs=True) for w in words]
    run = run_host(block, core1, accesses, timing=MODES[mode], tail_ns=10000)
    assert not run.errors, run.errors
    assert [v for (_, _, v) in core1.writes] == words
    assert not block.irq_flags & (1 << 4)
    waits = [r.wait_ns for r in run.results if r.wait_ns is not None]
    assert waits and max(waits) < Mode0().iordy_max


def test_data_read_with_no_block(programs):
    """A data read with no block under way (a host that ignores DRQ): the
    host isn't held (IORDY released as for any data read), DD isn't left
    driven, and the next block, started as core 1 starts every block, is
    served from its first word."""
    words = words_for(256, 9)
    core1 = LaneCore1(250, [words], start_on_command=False)
    block = make_block(programs)
    run = run_host(block, core1, [Access(False, "CMD", 0), Access(False, "CMD", 7)], timing=Mode0())
    assert not run.errors, run.errors
    stray, status = run.results
    assert stray.wait_ns is not None and stray.wait_ns < Mode0().iordy_max
    assert stray.read_value is None, "DD driven with no word to give"
    assert not block.pindirs & 0xFFFF
    check_read(status, STATUS_READY, Mode0())
    # ide_data_in took the IRQ and waits for a word: the next block's restart
    # clears that, so its first word goes to its first read.
    core1._start_pending = True
    accesses = [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)]
    run = run_host(block, core1, accesses, timing=Mode0())
    assert not run.errors, run.errors
    assert [r.read_value for r in run.results] == words


def test_chip_select_without_strobe(programs):
    """IORDY low while a chip select lasts, released when it goes, nothing
    reaching core 1 or the lane. (The lane has no watchdog: ISA cards keep
    ide_bus with `isa = on`.)"""
    block = make_block(programs)
    idle = (1 << 19) | (1 << 20) | (1 << 22) | (1 << 23) | (1 << IORDY_BIT) | (1 << 26)
    history, iordy = [], []
    for k in range(400):
        t_ns = k * CYCLE_PS / 1000
        pins = idle & ~(1 << 19) if 100 <= t_ns < 1500 else idle
        history.append(pins)
        block.step(synced(history, k, block.bypass_mask))
        iordy.append((t_ns, bool((block.pindirs >> IORDY_BIT) & 1)))
    low = [t for t, l in iordy if l]
    assert low and min(low) < 100 + 30 + 35 - IORDY_MARGIN_NS
    assert max(low) < 1500 + 100
    assert not block.rx_fifo and not block.irq_flags
