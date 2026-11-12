"""The register table (src/ide_table.pio, docs/register-table.md), co-simulated
with the ATA PIO host and models of its DMA channels. No hardware.

Every read is answered at its strobe by the PIO alone: the registers from
R0-R2's FIFO registers, data words through R0's entry 0, which a DMA pair
refills after each data read. So a host that never waits for IORDY must read
every register and every data word right, at PIO mode 0's minimum timings and
at the PIIX's compatible timing (no t1, 175 ns strobes), and a host that waits
must see IORDY low at tA in every mode, 4 included. After a data block's last
word the busy Status must be there for the host's next Status read, with no
core 1 involved.
"""
from __future__ import annotations

import dataclasses

import pytest

from hddproto.ide_host_model import FAST_PIO_MODES, PIO_MODES, Access, Core1, Mode0, run_host
from hddproto.pio_sim import CYCLE_PS, PioBlock, PioSim

pytestmark = pytest.mark.sim

DIOR_BIT, IORDY_BIT = 22, 24
DATA_ADDRESS = 16                  # DA2-0 = 0, CS0# low, CS1# high: D's Y
CLAIMS = {"R0": 4, "R1": 5, "R2": 3}  # DA2, CS0#, CS1#, as ide_table.cpp loads ISR
DRIVE = 0xFFFF0000
BUSY = DRIVE | 0x80                # the busy Status entry
ALL_MODES = {**PIO_MODES, **FAST_PIO_MODES}
NO_WAIT = {m: dataclasses.replace(t, waits_for_iordy=False) for m, t in PIO_MODES.items()}
# A PIIX in compatible timing (docs/no-iordy.md): chip select and strobe
# together, a short strobe, and IORDY ignored.
PIIX = dataclasses.replace(Mode0(), t1=0, t2=175, t0=600, waits_for_iordy=False)
# The Innostor USB bridge before it sets a mode (docs/register-table.md).
INNOSTOR = dataclasses.replace(Mode0(), t1=170, t2=600, t0=1000, waits_for_iordy=False)

REGISTERS = {  # (block, DA): value
    ("CMD", 1): 0x01, ("CMD", 2): 0x01, ("CMD", 3): 0x01, ("CMD", 4): 0x00,
    ("CMD", 5): 0x00, ("CMD", 6): 0xA0, ("CMD", 7): 0x50, ("CTL", 6): 0x50,
}


@pytest.fixture(scope="module")
def programs(assemble):
    return assemble("ide_table", "ide_table_reg"), assemble("ide_table", "ide_table_data")


class Table:
    """The four state machines, configured as ide_table.cpp configures them,
    with the register image loaded and the DMA channels modelled."""

    def __init__(self, programs, registers=REGISTERS, block_words: list[int] | None = None,
                 dma_latency: int = 4):
        reg_program, data_program = programs
        bypass = 0  # every pin through the synchroniser: a strobe and its address arrive together (no t1: the PIIX)
        self.r = {}
        for name, claim in CLAIMS.items():
            sm = PioSim(reg_program, out_base=0, out_count=16, bypass_mask=bypass)
            sm.isr = claim
            self.r[name] = sm
        self.d = PioSim(data_program, out_base=0, out_count=16, sideset_base=IORDY_BIT,
                        fifo_depth=4, tx_depth=4, bypass_mask=bypass)
        self.d.y = DATA_ADDRESS
        self.load(registers)
        # R2's addresses that aren't the drive's: never driven
        self.r["R2"].rxf[0] = self.r["R2"].rxf[1] = 0
        self.block = PioBlock(self.r["R0"], self.r["R1"], self.r["R2"], self.d)
        self.block.on_cycle = self.dma
        self.latency = dma_latency
        self.words = list(block_words or [])
        self.data_next = 1                # the next word for R0's entry 0
        self.ctrl = [0] * len(self.words) + [0] * 4
        if self.words:
            self.ctrl[len(self.words) - 1] = BUSY
            self.r["R0"].rxf[0] = DRIVE | self.words[0]
            self.d.tx_fifo = self.ctrl[:4]
        self.ctrl_next = 4
        self._feed_wait = None            # cycles left before the next data/control pair lands
        self._busy_wait = None
        self.busy_written_at = None

    def load(self, registers):
        entry = {("CMD", 1): ("R0", 1), ("CMD", 2): ("R0", 2), ("CMD", 3): ("R0", 3),
                 ("CMD", 4): ("R1", 0), ("CMD", 5): ("R1", 1), ("CMD", 6): ("R1", 2),
                 ("CMD", 7): ("R1", 3), ("CTL", 6): ("R2", 2), ("CTL", 7): ("R2", 3)}
        for key, value in registers.items():
            sm, i = entry[key]
            self.r[sm].rxf[i] = DRIVE | value

    def dma(self, block):
        # The data pair: D's pull made room in its TX FIFO; a word for R0's
        # entry 0, then the next control word, `latency` cycles each.
        if self._feed_wait is None and len(self.d.tx_fifo) < 4 and self.ctrl_next < len(self.ctrl):
            self._feed_wait = self.latency
        if self._feed_wait is not None:
            self._feed_wait -= 1
            if self._feed_wait == self.latency // 2 and self.data_next < len(self.words):
                self.r["R0"].rxf[0] = DRIVE | self.words[self.data_next]
                self.data_next += 1
            if self._feed_wait <= 0:
                self.d.tx_fifo.append(self.ctrl[self.ctrl_next])
                self.ctrl_next += 1
                self._feed_wait = None
        # The busy pair: D pushed the busy Status entry.
        if self._busy_wait is None and self.d.rx_fifo:
            self._busy_wait = self.latency
        if self._busy_wait is not None:
            self._busy_wait -= 1
            if self._busy_wait <= 0:
                word = self.d.rx_fifo.pop(0)
                self.r["R1"].rxf[3] = word
                self.r["R2"].rxf[2] = word
                self._busy_wait = None
                self.busy_written_at = block.cycle


def idle_core1() -> Core1:
    """Nothing reaches core 1 from block 0 in this mode."""
    return Core1(latency_ns=0, read_value=lambda block, addr: 0)


def register_reads() -> list[Access]:
    return [Access(False, block, addr) for (block, addr) in REGISTERS]


def check_registers(run):
    for r in run.results:
        want = REGISTERS[(r.access.block, r.access.addr)]
        assert r.read_value is not None and r.read_value & 0xFF == want, (
            f"{r.access.block} {r.access.addr}: read {r.read_value}, want {want:02x}")


@pytest.mark.parametrize("timing", [*NO_WAIT.values(), PIIX, INNOSTOR],
                         ids=[*(f"mode{m}-no-wait" for m in NO_WAIT), "piix", "innostor"])
def test_a_host_that_never_waits_reads_every_register(programs, timing):
    table = Table(programs)
    run = run_host(table.block, idle_core1(), register_reads() * 2, timing=timing)
    assert not run.errors
    check_registers(run)


@pytest.mark.parametrize("mode", ALL_MODES)
def test_a_host_that_waits_sees_iordy_at_ta_and_reads_every_register(programs, mode):
    table = Table(programs)
    run = run_host(table.block, idle_core1(), register_reads(), timing=ALL_MODES[mode])
    assert not run.errors
    check_registers(run)
    for r in run.results:
        assert r.iordy_low_at_tA, f"{r.access}: IORDY not low at tA"
        assert r.wait_ns is not None and r.wait_ns < 150, f"{r.access}: held {r.wait_ns} ns"
        assert r.release_ns is not None and r.release_ns <= ALL_MODES[mode].t6z, f"{r.access}: released after {r.release_ns} ns"


def test_addresses_that_arent_the_drives_are_never_driven(programs):
    table = Table(programs)
    reads = [Access(False, "CTL", a) for a in range(6)]  # an ISA card's floppy registers, 3F0-3F5
    run = run_host(table.block, idle_core1(), reads, timing=NO_WAIT[0])
    assert not run.errors
    assert all(r.read_value is None for r in run.results)


def test_a_changed_register_is_read_fresh_at_the_next_strobe(programs):
    # The Innostor bridge holds its chip select between reads, so an answer
    # prepared at the chip select belonged to the read before. Here the value
    # changes between two back-to-back Status reads and the second must see it.
    table = Table(programs)
    reads = [Access(False, "CMD", 7, hold_cs=True), Access(False, "CMD", 7)]
    changed = {"done": False}
    original = table.block.on_cycle

    def change_after_first(block):
        original(block)
        if not changed["done"] and table.d.pc == 0 and block.cycle > 60:
            table.r["R1"].rxf[3] = DRIVE | 0x58
            changed["done"] = True

    table.block.on_cycle = change_after_first
    run = run_host(table.block, idle_core1(), reads, timing=INNOSTOR)
    assert not run.errors
    assert [r.read_value & 0xFF for r in run.results] == [0x50, 0x58]


def sector(seed: int) -> list[int]:
    return [(seed * 251 + i * 7919) & 0xFFFF for i in range(256)]


@pytest.mark.parametrize("timing", [NO_WAIT[0], PIIX, *ALL_MODES.values()],
                         ids=["mode0-no-wait", "piix", *(f"mode{m}" for m in ALL_MODES)])
def test_a_data_block_reads_right_then_status_shows_busy(programs, timing):
    words = sector(3)
    table = Table(programs, block_words=words)
    reads = [Access(False, "CMD", 0, hold_cs=True) for _ in range(255)] + [Access(False, "CMD", 0)]
    reads += [Access(False, "CTL", 6), Access(False, "CMD", 7)]  # the host checks status after the block
    run = run_host(table.block, idle_core1(), reads, timing=timing)
    assert not run.errors
    got = [r.read_value for r in run.results[:256]]
    bad = [i for i, (g, w) in enumerate(zip(got, words)) if g != w]
    assert not bad, f"{len(bad)} words wrong, first at {bad[0]}: {got[bad[0]]} want {words[bad[0]]:04x}"
    assert [r.read_value & 0xFF for r in run.results[256:]] == [0x80, 0x80]


def test_busy_is_written_well_inside_the_hosts_400_ns(programs):
    words = sector(5)
    table = Table(programs, block_words=words)
    reads = [Access(False, "CMD", 0, hold_cs=True) for _ in range(255)] + [Access(False, "CMD", 0)]
    run = run_host(table.block, idle_core1(), reads, timing=ALL_MODES[4])
    assert not run.errors
    last_rise_cycle = run.results[-1].rise_ns * 1000 / CYCLE_PS
    assert table.busy_written_at is not None
    assert (table.busy_written_at * CYCLE_PS / 1000) - run.results[-1].fall_ns < 400
    assert table.busy_written_at < last_rise_cycle + 400_000 / CYCLE_PS


# ---- The write side: block 1 (src/ide_table_write.pio) ----------------------------

COMMAND_ADDRESS = 23  # DA2-0 = 7, CS0# low, CS1# high: C's Y


@pytest.fixture(scope="module")
def write_programs(assemble):
    return (assemble("ide_table_write", "ide_table_write"), assemble("ide_table_write", "ide_table_data_out"),
            assemble("ide_table_write", "ide_table_command"))


class Writes:
    """Block 1's W, WD and C, as ide_table.cpp configures them, beside a
    Table, with C's and WD's DMA chains writing the busy Status into it."""

    def __init__(self, write_programs, table: Table, data_block: int = 256, dma_latency: int = 4):
        w_program, wd_program, c_program = write_programs
        self.w = PioSim(w_program, autopush=True, fifo_depth=8)
        self.wd = PioSim(wd_program, autopush=True, push_threshold=16, in_shift_left=True, fifo_depth=8)
        self.wd.y = DATA_ADDRESS
        self.c = PioSim(c_program, fifo_depth=4)
        self.c.y = COMMAND_ADDRESS
        self.block = PioBlock(self.w, self.wd, self.c)
        self.block.on_cycle = self.dma
        self.table = table
        self.latency = dma_latency
        self.ring: list[int] = []        # W's DMA ring
        self.buffer: list[int] = []      # WD's channel: the sector buffer
        self.data_block = data_block
        self._busy_wait = None
        self.busy_written_at = None

    def dma(self, block):
        while self.w.rx_fifo:
            self.ring.append(self.w.rx_fifo.pop(0))
        start_busy = False
        while self.wd.rx_fifo and len(self.buffer) < self.data_block:
            self.buffer.append(self.wd.rx_fifo.pop(0) & 0xFFFF)
            if len(self.buffer) == self.data_block:
                start_busy = True        # the channel's done: it chains to the busy write
        if self.c.rx_fifo:
            self.c.rx_fifo.pop(0)
            start_busy = True
        if start_busy and self._busy_wait is None:
            self._busy_wait = 2 * self.latency  # two hops: the trigger, then the constant
        if self._busy_wait is not None:
            self._busy_wait -= 1
            if self._busy_wait <= 0:
                self.table.r["R1"].rxf[3] = BUSY
                self.table.r["R2"].rxf[2] = BUSY
                self._busy_wait = None
                self.busy_written_at = block.cycle


def decode_write(sample: int) -> tuple[str, int, int]:
    block = {(1, 0): "CMD", (0, 1): "CTL"}.get((((sample >> 19) & 1) ^ 1, ((sample >> 20) & 1) ^ 1), "NONE")
    return block, (sample >> 16) & 7, sample & 0xFFFF


@pytest.mark.parametrize("mode", ALL_MODES)
def test_every_write_reaches_the_ring_as_ide_bus_samples_it(programs, write_programs, mode):
    table = Table(programs)
    writes = Writes(write_programs, table)
    accesses = [Access(True, "CMD", a, 0x11 * a + 0x20) for a in range(1, 7)] + [Access(True, "CTL", 6, 0x0A)]
    run = run_host(table.block, idle_core1(), accesses, timing=ALL_MODES[mode], observers=(writes.block,))
    assert not run.errors
    assert [decode_write(s) for s in writes.ring] == [(a.block, a.addr, a.data) for a in accesses]
    assert all(not (s >> 23) & 1 for s in writes.ring)  # DIOW# bit 0: a write, as ide_bus's samples


@pytest.mark.parametrize("mode", ALL_MODES)
def test_a_data_block_written_lands_in_the_buffer_then_status_shows_busy(programs, write_programs, mode):
    table = Table(programs)
    writes = Writes(write_programs, table)
    data = sector(9)
    accesses = [Access(True, "CMD", 0, w, hold_cs=True) for w in data[:-1]] + [Access(True, "CMD", 0, data[-1])]
    accesses += [Access(False, "CTL", 6)]
    run = run_host(table.block, idle_core1(), accesses, timing=ALL_MODES[mode], observers=(writes.block,))
    assert not run.errors
    assert writes.buffer == data
    assert run.results[-1].read_value & 0xFF == 0x80


@pytest.mark.parametrize("timing", [NO_WAIT[0], PIIX, ALL_MODES[3], ALL_MODES[4]],
                         ids=["mode0-no-wait", "piix", "mode3", "mode4"])
def test_status_shows_busy_400_ns_after_a_command_write(programs, write_programs, timing):
    table = Table(programs)
    writes = Writes(write_programs, table)
    accesses = [Access(True, "CMD", 7, 0x20), Access(False, "CMD", 7), Access(False, "CTL", 6)]
    run = run_host(table.block, idle_core1(), accesses, timing=timing, observers=(writes.block,))
    assert not run.errors
    command, status, alt = run.results
    assert writes.busy_written_at is not None
    busy_ns = writes.busy_written_at * CYCLE_PS / 1000
    assert busy_ns - command.rise_ns < 400, f"busy {busy_ns - command.rise_ns:.0f} ns after the write"
    if status.fall_ns - command.rise_ns >= 400:  # as ATA lets the host read it
        assert status.read_value & 0xFF == 0x80 and alt.read_value & 0xFF == 0x80


def test_reads_never_reach_the_write_side(programs, write_programs):
    table = Table(programs)
    writes = Writes(write_programs, table)
    run = run_host(table.block, idle_core1(), register_reads(), timing=ALL_MODES[0], observers=(writes.block,))
    assert not run.errors
    assert writes.ring == [] and writes.buffer == [] and writes.busy_written_at is None
