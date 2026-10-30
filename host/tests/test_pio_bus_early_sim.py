"""The early-answer bus program (src/ide_bus.pio's ide_bus_early,
`early_answers = on`, docs/no-iordy.md), co-simulated with ATA PIO hosts:
ones that wait for IORDY (modes 0-3), and ones that don't, which end every
strobe after t2 whatever IORDY says (an Intel PIIX in its compatible timing).
Core 1 answers at the chip select, so a host that doesn't wait still gets
its data, if core 1 is quick enough. No hardware needed."""
from __future__ import annotations

from dataclasses import replace

import pytest

from hddproto.ide_host_model import FAST_PIO_MODES, PIO_MODES, Access, Core1, Mode0, run_host
from hddproto.pio_sim import CYCLE_PS, PioSim

pytestmark = pytest.mark.sim

MODES = {**PIO_MODES, 3: FAST_PIO_MODES[3]}
NO_IORDY = replace(Mode0(), waits_for_iordy=False)  # PIO mode 0's minimum timings, IORDY ignored
B_DIOR, B_DIOW = 22, 23
DIOR_BIT = 22


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("ide_bus", program="ide_bus_early")


def make_sim(program) -> PioSim:
    """Configured as ide_bus.cpp configures the state machine."""
    sim = PioSim(program, out_base=0, out_count=16, sideset_base=24, jmp_pin=DIOR_BIT,
                 fifo_depth=4, tx_depth=4, bypass_mask=1 << DIOR_BIT, status_rx_below=4, autopush=True)
    sim.pc = program.offsets["idle"]
    return sim


class EarlyCore1(Core1):
    """Core 1 for the early program: answers requests (both strobe bits 0)
    with the register's current value, and applies reads' side effects when
    their samples come (a data read moves on to the next word)."""

    def __init__(self, latency_ns, registers=None, data=None, service_ns=0):
        super().__init__(latency_ns, lambda b, a: 0, service_ns)
        self.registers = dict(registers or {})
        self.data = list(data or [])
        self.data_index = 0
        self.requests = 0
        self.reads: list[tuple[str, int]] = []

    def value(self, block, addr):
        if (block, addr) == ("CMD", 0):
            return self.data[self.data_index] if self.data_index < len(self.data) else 0
        return self.registers.get((block, addr), 0)

    def on_word(self, t_ps, sample):
        from hddproto.ide_host_model import B_CS0, B_CS1, B_DA0, DD_MASK
        block = {(1, 0): "CMD", (0, 1): "CTL"}.get(
            (((sample >> B_CS0) & 1) ^ 1, ((sample >> B_CS1) & 1) ^ 1), "NONE")
        addr = (sample >> B_DA0) & 7
        strobes = (sample >> B_DIOR) & 3
        if strobes == 0:  # an answer request
            self.requests += 1
            ours = block == "CMD" or (block == "CTL" and addr == 6)
            self._pending.append((t_ps + self.latency_ps, (0xFFFF0000 if ours else 0) | self.value(block, addr)))
        elif not (sample >> B_DIOW) & 1:  # a write
            self.writes.append((block, addr, sample & DD_MASK))
            if block == "CMD" and addr not in (0, 7):  # 7 is COMMAND on a write, STATUS on a read
                self.registers[(block, addr)] = sample & 0xFF
        else:  # a read happened
            self.reads.append((block, addr))
            if (block, addr) == ("CMD", 0):
                self.data_index += 1


def identify_burst(core1_latency_ns, timing):
    words = [0x1000 + i for i in range(256)]
    core1 = EarlyCore1(core1_latency_ns, {("CMD", 7): 0x58, ("CTL", 6): 0x58}, words)
    accesses = [Access(True, "CMD", 6, 0xA0), Access(True, "CMD", 7, 0xEC), Access(False, "CTL", 6),
                Access(False, "CMD", 7)]
    accesses += [Access(False, "CMD", 0, hold_cs=i < 255) for i in range(256)]
    return core1, words, accesses


def read_values(run):
    return [r.read_value for r in run.results if not r.access.write]


@pytest.mark.parametrize("latency_ns", [100, 150])
def test_no_iordy_host_reads_registers_and_identify(program, latency_ns):
    """The Pentium's case: a host that never waits. With core 1 answering
    within 150 ns of the chip select, every read is right."""
    core1, words, accesses = identify_burst(latency_ns, NO_IORDY)
    run = run_host(make_sim(program), core1, accesses, timing=NO_IORDY)
    assert not run.errors, run.errors
    assert read_values(run) == [0x58, 0x58] + words
    assert core1.reads[:2] == [("CTL", 6), ("CMD", 7)] and len(core1.reads) == 258
    assert core1.writes == [("CMD", 6, 0xA0), ("CMD", 7, 0xEC)]


def test_no_iordy_host_reads_back_a_write(program):
    """The Phoenix BIOS's check: write DEVICE, read it back."""
    core1 = EarlyCore1(150, {("CMD", 7): 0x50})
    accesses = [Access(True, "CMD", 6, 0xA0), Access(False, "CMD", 7), Access(True, "CMD", 6, 0xB0),
                Access(False, "CMD", 6)]
    run = run_host(make_sim(program), core1, accesses, timing=NO_IORDY)
    assert not run.errors, run.errors
    assert read_values(run) == [0x50, 0xB0]


def test_no_iordy_core1_budget(program):
    """How late core 1 may answer a no-IORDY host in PIO mode 0's minimum
    timing: the first latency at which a read goes wrong."""
    first_bad = None
    for latency in range(100, 400, 10):
        core1 = EarlyCore1(latency, {("CMD", 7): 0x50})
        run = run_host(make_sim(program), core1, [Access(False, "CMD", 7)], timing=NO_IORDY)
        if read_values(run) != [0x50]:
            first_bad = latency
            break
    print(f"\nno-IORDY host, PIO mode 0: core 1 may take up to {first_bad - 10} ns from the chip select")
    assert first_bad is not None and first_bad > 150


@pytest.mark.parametrize("mode", list(MODES))
def test_waiting_hosts_still_work(program, mode):
    """Hosts that wait for IORDY (the USB adapter): correct data in modes 0-3,
    with core 1 as slow as it is today."""
    timing = MODES[mode]
    core1, words, accesses = identify_burst(400, timing)
    run = run_host(make_sim(program), core1, accesses, timing=timing)
    assert not run.errors, run.errors
    assert read_values(run) == [0x58, 0x58] + words


@pytest.mark.parametrize("mode", list(MODES))
def test_address_change_while_selected(program, mode):
    """CS0# held from a status read to a data read (DA changes, the chip select
    doesn't): the early answer for STATUS is thrown away, DATA is asked for."""
    timing = MODES[mode]
    core1 = EarlyCore1(150, {("CMD", 7): 0x58}, [0x1234, 0x5678])
    accesses = [Access(False, "CMD", 7, hold_cs=True), Access(False, "CMD", 0, hold_cs=True),
                Access(False, "CMD", 0)]
    run = run_host(make_sim(program), core1, accesses, timing=timing)
    assert not run.errors, run.errors
    assert read_values(run) == [0x58, 0x1234, 0x5678]


@pytest.mark.parametrize("mode", list(MODES))
def test_write_burst_with_a_slow_core1(program, mode):
    """WRITE SECTORS: flow control still holds a host that waits."""
    words = [(i * 7919) & 0xFFFF for i in range(256)]
    core1 = EarlyCore1(200, service_ns=1000)
    accesses = [Access(True, "CMD", 0, w, hold_cs=True) for w in words]
    run = run_host(make_sim(program), core1, accesses, timing=MODES[mode], tail_ns=20000)
    assert not run.errors, run.errors
    assert [v for (_, _, v) in core1.writes] == words


def test_chip_select_without_strobe(program):
    """A chip select that comes and goes with no strobe: one request, its
    answer thrown away, and the next access is still right."""
    from hddproto.pio_sim import synced
    core1 = EarlyCore1(150, {("CMD", 7): 0x50})
    sim = make_sim(program)
    idle = (1 << 19) | (1 << 20) | (1 << 22) | (1 << 23) | (1 << 24) | (1 << 26)
    history = []
    for k in range(400):
        t_ns = k * CYCLE_PS / 1000
        history.append(idle & ~(1 << 19) if 100 <= t_ns < 600 else idle)
        sim.step(synced(history, k, sim.bypass_mask))
        while sim.rx_fifo:
            core1.on_word(k * CYCLE_PS, sim.rx_fifo.pop(0))
        core1.deliver(k * CYCLE_PS, sim)
    assert core1.requests == 1 and not core1.reads
    run = run_host(sim, core1, [Access(False, "CMD", 7)], timing=NO_IORDY, start_ns=2100)
    assert not run.errors, run.errors
    assert read_values(run) == [0x50]


def test_no_iordy_write_with_a_late_answer(program):
    """A host that doesn't wait writes registers while core 1 answers each
    request 300 ns after it, after the write strobe has ended. The writes must
    still be sampled as DIOW# rises, not after the late answer (0.23.8). (Much
    later, about 600 ns, and the program would miss the next write while it
    waits to throw the answer away: core 1 answers from its tight loop.)"""
    core1 = EarlyCore1(300, {("CMD", 7): 0x50})
    values = [(3, 0x12), (4, 0x34), (5, 0x56), (6, 0xE0)]
    accesses = [Access(True, "CMD", addr, v) for addr, v in values]
    run = run_host(make_sim(program), core1, accesses, timing=NO_IORDY, tail_ns=5000)
    assert not run.errors, run.errors
    assert core1.writes == [("CMD", addr, v) for addr, v in values]
