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
from hddproto.pio_sim import CYCLE_PS, DmaToTx, PioBlock, PioSim

pytestmark = pytest.mark.sim

DIOR_BIT, IORDY_BIT = 22, 24
ADDRESS_BIT, ADDRESS_PINS = 16, 5    # IN_BASE (DA0) and IN_COUNT, for R0-R2 and D
DATA_ADDRESS = 16                  # DA2-0 = 0, CS0# low, CS1# high: D's Y
DRIVE = 0x00FF0000                 # DD0-7 only, as a drive answers its 8-bit registers


def at(block: str, addr: int) -> int:
    """An entry's address (bits 8-15), as a pin read gives it: DA0-2, CS0#, CS1#."""
    return ((16 if block == "CMD" else 8) + addr) << 8


STATUS_AT, ALT_AT = at("CMD", 7), at("CTL", 6)
BUSY = DRIVE | 0xD0                # the busy Status entry: BSY, DRDY, DSC
SPARE = 0xDEAD                     # the word the stream adds after each block
ALL_MODES = {**PIO_MODES, **FAST_PIO_MODES}
NO_WAIT = {m: dataclasses.replace(t, waits_for_iordy=False) for m, t in PIO_MODES.items()}
# A PIIX in compatible timing (docs/no-iordy.md): chip select and strobe
# together, a short strobe, and IORDY ignored.
PIIX = dataclasses.replace(Mode0(), t1=0, t2=175, t0=600, waits_for_iordy=False)
# The Innostor USB bridge before it sets a mode (docs/register-table.md).
INNOSTOR = dataclasses.replace(Mode0(), t1=170, t2=600, t0=1000, waits_for_iordy=False)
# The Innostor once it has set PIO mode 4: its first Status read after a
# Command write is a 75 ns strobe at the same address, not waiting for IORDY
# (12 Nov 2026, the observer's strobe lengths).
INNOSTOR_MODE4 = dataclasses.replace(ALL_MODES[4], t2=75, waits_for_iordy=False)

REGISTERS = {  # (block, DA): value
    ("CMD", 1): 0x01, ("CMD", 2): 0x01, ("CMD", 3): 0x01, ("CMD", 4): 0x00,
    ("CMD", 5): 0x00, ("CMD", 6): 0xA0, ("CMD", 7): 0x50, ("CTL", 6): 0x50,
}


@pytest.fixture(scope="module")
def programs(assemble):
    return assemble("ide_table", "ide_table_reg"), assemble("ide_table", "ide_table_data")


class Table:
    """The four state machines, configured as ide_table.cpp configures them,
    with the register image loaded and the DMA channels modelled: the stream
    of the block (and a spare word) into D's 8-deep TX FIFO, and on its
    completion the busy write into R1's Status and R2's Alternate Status."""

    def __init__(self, programs, registers=REGISTERS, block_words: list[int] | None = None,
                 dma_latency: int = 4):
        reg_program, data_program = programs
        bypass = 1 << DIOR_BIT  # DIOR# skips the synchroniser in block 0 (ide_table.cpp); the address doesn't
        self.r = {}
        for name in ("R0", "R1", "R2"):
            self.r[name] = PioSim(reg_program, out_base=0, out_count=16, bypass_mask=bypass, jmp_pin=DIOR_BIT,
                                  in_base=ADDRESS_BIT, in_count=ADDRESS_PINS)
        self.d = PioSim(data_program, out_base=0, out_count=16, sideset_base=IORDY_BIT,
                        fifo_depth=4, tx_depth=8, bypass_mask=bypass, jmp_pin=DIOR_BIT, in_base=ADDRESS_BIT,
                        in_count=ADDRESS_PINS)
        self.d.y = DATA_ADDRESS
        self.d.pc = data_program.offsets["top"]  # as ide_table.cpp starts it
        self.load(registers)
        # Empty entries: the data register (D's), and R2's addresses that aren't the drive's
        self.r["R0"].rxf[0] = 0
        self.r["R2"].rxf[0] = self.r["R2"].rxf[1] = 0
        self.block = PioBlock(self.r["R0"], self.r["R1"], self.r["R2"], self.d)
        self.block.on_cycle = self.dma
        self.latency = dma_latency
        self.words = list(block_words or [])
        self.stream = DmaToTx(self.d, self.words + [SPARE], latency=dma_latency) if self.words else None
        self._busy_wait = None
        self.busy_written_at = None

    def load(self, registers):
        entry = {("CMD", 1): ("R0", 1), ("CMD", 2): ("R0", 2), ("CMD", 3): ("R0", 3),
                 ("CMD", 4): ("R1", 0), ("CMD", 5): ("R1", 1), ("CMD", 6): ("R1", 2),
                 ("CMD", 7): ("R1", 3), ("CTL", 6): ("R2", 2), ("CTL", 7): ("R2", 3)}
        for key, value in registers.items():
            sm, i = entry[key]
            self.r[sm].rxf[i] = DRIVE | value | at(*key)

    def host_done(self) -> bool:
        """What core 1 checks: the stream done and only the spare word left."""
        return self.stream is not None and not self.stream.busy and len(self.d.tx_fifo) <= 1

    def dma(self, block):
        if self.stream is None:
            return
        was_busy = self.stream.busy
        self.stream(block)
        if was_busy and not self.stream.busy:  # the stream's completion chains to the busy write
            self._busy_wait = 2 * self.latency
        if self._busy_wait is not None:
            self._busy_wait -= 1
            if self._busy_wait <= 0:
                self.r["R1"].rxf[3] = BUSY | STATUS_AT
                self.r["R2"].rxf[2] = BUSY | ALT_AT
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
        # R0-R2 look at DIOR# once per pass of `hold`, 6 cycles: at worst 35 ns
        # against t6z's 30, well inside the 70 ns before a PIO 3 host's next strobe.
        assert r.release_ns is not None and r.release_ns <= ALL_MODES[mode].t6z + 5, f"{r.access}: released after {r.release_ns} ns"


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
        if not changed["done"] and table.d.pc == table.d.program.offsets["top"] and block.cycle > 60:
            table.r["R1"].rxf[3] = DRIVE | 0x58 | STATUS_AT
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
    assert [r.read_value & 0xFF for r in run.results[256:]] == [0xD0, 0xD0]


def test_core1_sees_the_block_done_only_after_the_last_word(programs):
    words = sector(5)
    table = Table(programs, block_words=words)
    reads = [Access(False, "CMD", 0, hold_cs=True) for _ in range(255)]
    run = run_host(table.block, idle_core1(), reads, timing=ALL_MODES[4])
    assert not run.errors
    assert not table.host_done()  # one word still to read
    table2 = Table(programs, block_words=words)
    run = run_host(table2.block, idle_core1(), reads + [Access(False, "CMD", 0)], timing=ALL_MODES[4])
    assert not run.errors and table2.host_done()
    assert table2.busy_written_at is not None
    assert table2.busy_written_at * CYCLE_PS / 1000 < run.results[-1].rise_ns + 400


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
                self.table.r["R1"].rxf[3] = BUSY | STATUS_AT
                self.table.r["R2"].rxf[2] = BUSY | ALT_AT
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
    assert run.results[-1].read_value & 0xFF == 0xD0


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
        assert status.read_value & 0xFF == 0xD0 and alt.read_value & 0xFF == 0xD0


def test_reads_never_reach_the_write_side(programs, write_programs):
    table = Table(programs)
    writes = Writes(write_programs, table)
    run = run_host(table.block, idle_core1(), register_reads(), timing=ALL_MODES[0], observers=(writes.block,))
    assert not run.errors
    assert writes.ring == [] and writes.buffer == [] and writes.busy_written_at is None


@pytest.mark.parametrize("mode", ALL_MODES)
def test_data_reads_never_hold_iordy(programs, mode):
    # The Innostor bridge's IORDY takes about a microsecond to rise once released:
    # a data read that found it low took 1.3 us, and the bridge gave up on sectors.
    words = sector(11)
    table = Table(programs, block_words=words)
    reads = [Access(False, "CMD", 0, hold_cs=True) for _ in range(255)] + [Access(False, "CMD", 0)]
    run = run_host(table.block, idle_core1(), reads, timing=ALL_MODES[mode])
    assert not run.errors
    assert [r.read_value for r in run.results] == words
    assert not any(r.iordy_low_at_tA for r in run.results)


@pytest.mark.parametrize("timing", [INNOSTOR_MODE4, dataclasses.replace(ALL_MODES[3], waits_for_iordy=False)],
                         ids=["innostor-mode4", "mode3-no-wait"])
def test_a_status_read_straight_after_the_command_write_sees_busy(programs, write_programs, timing):
    # The answer must be on DD before a short strobe ends: answered from the
    # strobe (about 90 ns), the bridge read the undriven bus, 7Fh, as ready
    # and took a block that wasn't there yet.
    table = Table(programs)
    writes = Writes(write_programs, table)
    accesses = [Access(True, "CMD", 7, 0x20, hold_cs=True)] + [Access(False, "CMD", 7, hold_cs=True)] * 3
    accesses += [Access(False, "CMD", 7)]
    run = run_host(table.block, idle_core1(), accesses, timing=timing, observers=(writes.block,))
    assert not run.errors
    values = [r.read_value for r in run.results[1:]]
    assert all(v is not None for v in values), f"undriven: {values}"
    assert [v & 0xFF for v in values] == [0xD0] * 4, [hex(v) for v in values]


@pytest.mark.parametrize("timing", [INNOSTOR_MODE4, dataclasses.replace(ALL_MODES[3], waits_for_iordy=False)],
                         ids=["innostor-mode4", "mode3-no-wait"])
def test_back_to_back_polls_with_short_strobes_read_every_change(programs, timing):
    # Status polls at one address, the value changing between them: each poll
    # sees the value as it was a few cycles before its strobe, never older.
    # The first follows a new address by only t1 (25-30 ns): from an address
    # change the answer takes about 95 ns, so only a host that waits for IORDY
    # (as every mode 3 and 4 host must) reads that one right.
    table = Table(programs)
    reads = [Access(False, "CMD", 7, hold_cs=True)] * 6 + [Access(False, "CMD", 7)]
    seen = []
    original = table.block.on_cycle

    def change(block):
        original(block)
        # a new value every 100 cycles (500 ns), as core 1 publishes
        table.r["R1"].rxf[3] = DRIVE | (0x50 + (block.cycle // 100) % 8) | STATUS_AT
        seen.append((block.cycle, table.r["R1"].rxf[3] & 0xFF))

    table.block.on_cycle = change
    run = run_host(table.block, idle_core1(), reads, timing=timing)
    assert not run.errors
    for r in run.results[1:]:
        assert r.read_value is not None, f"undriven at {r.fall_ns} ns"
        fall = int(r.fall_ns * 1000 / CYCLE_PS)
        # the value at some cycle from 8 before the strobe to the strobe's end
        window = {v for c, v in seen if fall - 8 <= c <= fall + int(timing.t2 * 1000 / CYCLE_PS)}
        assert r.read_value & 0xFF in window, (r.fall_ns, hex(r.read_value), window)


@pytest.mark.parametrize("timing", [dataclasses.replace(PIO_MODES[2], t1=0), dataclasses.replace(PIO_MODES[2], t1=5),
                                    dataclasses.replace(PIO_MODES[0], t1=0), PIIX],
                         ids=["pio2-t1=0", "pio2-t1=5", "pio0-t1=0", "piix"])
def test_the_first_data_read_after_status_polls_with_no_address_setup(programs, timing):
    # The Innostor bridge goes from its Status polls to the data with the
    # address changing as DIOR# falls (DA2 last): R1 answered LBA Mid (01Bh)
    # over D's first word (12 Nov 2026), and D, looking too early, could take
    # the read for a register and leave the word for the next read.
    words = sector(13)
    registers = {**REGISTERS, ("CMD", 4): 0x1B, ("CMD", 7): 0x58}
    table = Table(programs, registers=registers, block_words=words)
    accesses = [Access(False, "CMD", 7, hold_cs=True)] * 3
    accesses += [Access(False, "CMD", 0, hold_cs=True)] * 7 + [Access(False, "CMD", 0)]
    run = run_host(table.block, idle_core1(), accesses, timing=timing)
    assert not run.errors
    assert [r.read_value & 0xFF for r in run.results[:3]] == [0x58] * 3
    assert [r.read_value for r in run.results[3:]] == words[:8], [hex(v) if v is not None else None for v in
                                                                   (r.read_value for r in run.results[3:])]


# ---- The outputs follow the bus while DIOR# is low (13 Nov 2026) -------------------

LATE_HOSTS = {  # hosts whose strobe is long enough, or who wait, for an answer about 110 ns in
    "pio0": PIO_MODES[0], "pio2": PIO_MODES[2], "pio3": FAST_PIO_MODES[3], "innostor": INNOSTOR,
}


@pytest.mark.parametrize("lag", [5, 10, 15, 20, 30, 40])
@pytest.mark.parametrize("host", LATE_HOSTS)
def test_a_data_read_whose_address_settles_after_the_strobe(programs, host, lag):
    # (Up to 50 ns for a host that waits for IORDY and no longer: D takes its
    # last look with IORDY low 55 ns after the strobe.)
    # Decided once, 2 cycles after the strobe, a first data read whose address
    # settled 10 ns or more after DIOR# fell was answered as the Status before
    # it, or by nobody, and D kept its word: the block ran one word late and
    # never finished (what the Innostor bridge showed in PIO 3: its first data
    # read after each Command with IORDY low, as a register's).
    timing = dataclasses.replace(LATE_HOSTS[host], address_lag=lag)
    words = sector(13)
    registers = {**REGISTERS, ("CMD", 4): 0x1B, ("CMD", 7): 0x58}
    table = Table(programs, registers=registers, block_words=words)
    accesses = [Access(False, "CMD", 7, hold_cs=True)] * 3
    accesses += [Access(False, "CMD", 0, hold_cs=True)] * 7 + [Access(False, "CMD", 0)]
    run = run_host(table.block, idle_core1(), accesses, timing=timing)
    assert not run.errors
    assert [r.read_value & 0xFF for r in run.results[:3]] == [0x58] * 3
    assert [r.read_value for r in run.results[3:]] == words[:8]


def test_status_and_then_the_data_register_within_one_strobe(programs):
    # A host that leaves DIOR# low on Status and moves the address to the data
    # register under it: Status is driven, then let go, and the block's first
    # word follows, as from a drive (its outputs are gates on the address).
    timing = dataclasses.replace(INNOSTOR, address_lag=300)
    words = sector(7)
    table = Table(programs, registers={**REGISTERS, ("CMD", 7): 0x58}, block_words=words)
    accesses = [Access(False, "CMD", 7, hold_cs=True), Access(False, "CMD", 0, hold_cs=True),
                Access(False, "CMD", 0)]
    seen = []
    original = table.block.on_cycle

    def watch(block):
        original(block)
        seen.append((block.pindirs & 0xFF, block.pin_out & 0xFFFF))

    table.block.on_cycle = watch
    run = run_host(table.block, idle_core1(), accesses, timing=timing)
    assert not run.errors
    assert run.results[0].read_value & 0xFF == 0x58
    assert run.results[1].read_value == words[0]
    assert (0xFF, 0x58) in [(d, v & 0xFF) for d, v in seen]  # Status was on the bus first


def test_status_changes_under_a_long_strobe(programs):
    # A host that holds one Status strobe until the drive is ready sees the
    # register change under it.
    timing = dataclasses.replace(INNOSTOR, t2=2000, t0=2400)
    table = Table(programs, registers={**REGISTERS, ("CMD", 7): 0xD0})
    original = table.block.on_cycle

    def ready_mid_strobe(block):
        original(block)
        if block.cycle == 300:  # 1.5 us in: mid-strobe
            table.r["R1"].rxf[3] = DRIVE | 0x58 | STATUS_AT

    table.block.on_cycle = ready_mid_strobe
    run = run_host(table.block, idle_core1(), [Access(False, "CMD", 7)], timing=timing, timeout_ns=4000)
    assert not run.errors
    assert run.results[0].read_value & 0xFF == 0x58


# ---- DA2 settling late: a register's entry over another's (13 Nov 2026) -------------


@pytest.mark.parametrize("mode, skew", [(m, s) for m in (0, 2, 3, 4) for s in (0, 5, 10, 20, 25)] + [(0, 40), (0, 60)])
def test_status_after_data_writes_with_da2_settling_late(programs, mode, skew):
    # The JMicron adapter, after a sector's data writes, read Status as LBA Low
    # (D0h, F2h: the write's LBA Low) and reset the drive: going from the data
    # register (0) to Status (7) the address passes through LBA Low's (3) if
    # DA2 is last to settle, and R0, driving LBA Low for it, was still on DD
    # (the latch and output enables are shared) when the host sampled.
    # Skews inside t1 (the address valid before the strobe). Settling well after
    # it (60 ns in modes 2-3, 30 ns into the strobe) can still read undriven: the
    # R that let go clears DD's output enables after the right one drove them.
    timing = dataclasses.replace(ALL_MODES[mode], da2_skew=skew)
    registers = {**REGISTERS, ("CMD", 3): 0xF2, ("CMD", 7): 0x58, ("CTL", 6): 0x58}
    table = Table(programs, registers=registers)
    accesses = [Access(True, "CMD", 0, data=0x1234 + n, hold_cs=True) for n in range(4)]
    accesses += [Access(False, "CMD", 7, hold_cs=True), Access(False, "CTL", 6, hold_cs=True)]
    accesses += [Access(True, "CMD", 0, data=0x2222, hold_cs=True), Access(False, "CMD", 7)]
    run = run_host(table.block, idle_core1(), accesses, timing=timing)
    assert not run.errors
    reads = [r.read_value for r in run.results if not r.access.write]
    assert [v & 0xFF if v is not None else None for v in reads] == [0x58] * 3, [hex(v) if v is not None else v for v in reads]


@pytest.mark.parametrize("mode", [0, 2, 3, 4])
def test_iordy_falls_within_20_ns_of_a_register_read(programs, mode):
    # D watches the address before the strobe, so a register read gets IORDY low
    # about 10-20 ns after DIOR# falls: deciding after the strobe (30-35 ns) was
    # too late for the Innostor bridge in PIO 3, whose 75 ns Status reads after
    # a Command then sometimes latched DD before R1 drove it (13 Nov 2026).
    table = Table(programs)
    run = run_host(table.block, idle_core1(), register_reads(), timing=ALL_MODES[mode])
    assert not run.errors
    check_registers(run)
    leads = [r.iordy_low_lead_ns for r in run.results]
    assert all(lead is not None and lead >= -20 for lead in leads), leads
