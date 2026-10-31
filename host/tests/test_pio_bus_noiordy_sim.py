"""The no-IORDY bus program (src/ide_bus.pio's ide_bus_noiordy, `iordy = off`
with `early_answers = on`), co-simulated with a host like an Amiga 1200's
Gayle: DA2-0 and the chip selects are valid for 110-200 ns before each
strobe and change as it starts (or a little before), IORDY is never looked
at, and strobes last about 300 ns. Every read must reach the host with the
armed address's value, and core 1 must count each one, or the drive falls
behind the host (2026-10-31: IDENTIFY's words went missing). No hardware
needed."""
from __future__ import annotations

import pytest

from hddproto.ide_host_model import B_CS0, B_CS1, B_DA0, B_DIOR, B_DIOW, DD_MASK
from hddproto.pio_sim import CYCLE_PS, SYNC_CYCLES, PioSim, synced

pytestmark = pytest.mark.sim

NS = 1000  # ps


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("ide_bus", program="ide_bus_noiordy")


def make_sim(program) -> PioSim:
    """Configured as ide_bus.cpp configures the state machine (no side-set)."""
    sim = PioSim(program, out_base=0, out_count=16, jmp_pin=B_DIOR, fifo_depth=4, tx_depth=4,
                 bypass_mask=1 << B_DIOR, autopush=True)
    sim.pc = program.offsets["idle"]
    return sim


def address_bits(block: str, addr: int) -> int:
    cs0 = 0 if block == "CMD" else 1  # active low
    cs1 = 0 if block == "CTL" else 1
    return (addr << B_DA0) | (cs0 << B_CS0) | (cs1 << B_CS1)


IDLE = address_bits("NONE", 0)  # nothing selected
STROBES_OFF = (1 << B_DIOR) | (1 << B_DIOW)


class Gayle:
    """Runs reads (and writes) against the program as Gayle does them, with
    core 1 answering each request `latency_ns` after it reaches the FIFO."""

    def __init__(self, program, latency_ns=120, t1_ns=150, garbage_lead_ns=0, strobe_ns=300,
                 recovery_ns=300, garbage=0b11011):
        self.sim = make_sim(program)
        self.latency_ps = latency_ns * NS
        self.t1, self.lead, self.strobe, self.recovery = t1_ns * NS, garbage_lead_ns * NS, strobe_ns * NS, recovery_ns * NS
        self.garbage = garbage  # DA2-0, CS0#, CS1# as Gayle leaves them during a strobe
        self.history: list[int] = []
        self.t = 0
        self.pending: list[tuple[int, int]] = []
        self.registers = {("CMD", 7): 0x58, ("CTL", 6): 0x58}
        self.data: list[int] = []
        self.data_index = 0
        self.asked = IDLE
        self.reads_counted: list[int] = []  # the addresses core 1 counted reads for
        self.writes: list[tuple[int, int]] = []

    def value(self, address: int) -> int:
        if address == address_bits("CMD", 0):
            return self.data[self.data_index] if self.data_index < len(self.data) else 0
        for (block, addr), v in self.registers.items():
            if address == address_bits(block, addr):
                return v
        return 0

    def _core1(self, word: int):
        if (word >> B_DIOR) & 3 == 0:  # a request: answer it; remember its address if selected
            address = word & (0x1F << B_DA0)
            if (address >> B_CS0) & 3 != 3:  # as earlyAddress: no chip select, not an access
                self.asked = address
            self.pending.append((self.t + self.latency_ps, 0xFFFF0000 | self.value(address)))
        elif (word >> B_DIOW) & 1:  # a read's report: counted for the asked address
            self.reads_counted.append(self.asked)
            if self.asked == address_bits("CMD", 0):
                self.data_index += 1
        else:  # a write
            self.writes.append((self.asked, word & DD_MASK))

    def _run(self, duration_ps: int, wave_bits: int, dd_in: int | None = None) -> int:
        """Holds the host's lines for `duration_ps`; returns DD as the host
        sees it at the end (the drive's value where it drives, else high)."""
        end = self.t + duration_ps
        while self.t < end:
            pins = wave_bits | (dd_in if dd_in is not None else 0)
            k = len(self.history)
            self.history.append(pins)
            self.sim.step(synced(self.history, k, self.sim.bypass_mask))
            while self.sim.rx_fifo:
                self._core1(self.sim.rx_fifo.pop(0))
            while self.pending and self.pending[0][0] <= self.t and len(self.sim.tx_fifo) < 4:
                self.sim.tx_fifo.append(self.pending.pop(0)[1])
            self.t += CYCLE_PS
        driven = self.sim.pindirs & DD_MASK
        return (self.sim.pin_out & driven) | (DD_MASK & ~driven)

    def read(self, block: str, addr: int) -> int:
        a = address_bits(block, addr)
        self._run(self.t1 - self.lead, a | STROBES_OFF)
        if self.lead:
            self._run(self.lead, (self.garbage << B_DA0) | STROBES_OFF)
        value = self._run(self.strobe, (self.garbage << B_DA0) | (1 << B_DIOW))  # DIOR# low
        self._run(self.recovery, IDLE | STROBES_OFF)
        return value

    def write(self, block: str, addr: int, value: int):
        a = address_bits(block, addr)
        self._run(self.t1 - self.lead, a | STROBES_OFF, value)
        if self.lead:
            self._run(self.lead, (self.garbage << B_DA0) | STROBES_OFF, value)
        self._run(self.strobe, (self.garbage << B_DA0) | (1 << B_DIOR), value)  # DIOW# low
        self._run(self.recovery, IDLE | STROBES_OFF)


@pytest.mark.parametrize("t1_ns", [110, 200])
@pytest.mark.parametrize("lead_ns", [0, 20])
def test_identify_block_with_gayle_timing(program, t1_ns, lead_ns):
    """IDENTIFY's 256 words, the address gone as each strobe starts (or 20 ns
    before): every word right, and every read counted as a data read."""
    host = Gayle(program, t1_ns=t1_ns, garbage_lead_ns=lead_ns)
    host.data = [0x1000 + i for i in range(256)]
    assert host.read("CMD", 7) == 0x58
    words = [host.read("CMD", 0) for _ in range(256)]
    assert words == host.data
    assert host.data_index == 256
    assert host.reads_counted.count(address_bits("CMD", 0)) == 256


@pytest.mark.parametrize("lead_ns", [0, 20])
def test_registers_and_writes_with_gayle_timing(program, lead_ns):
    """Kickstart's register test: write LBA mid, read it back, alternate with
    status; writes land on the armed address."""
    host = Gayle(program, garbage_lead_ns=lead_ns)
    host.write("CMD", 4, 0x1212)
    host.registers[("CMD", 4)] = 0x12  # what core 1 would hold after the write
    assert host.read("CMD", 4) == 0x12
    assert host.read("CMD", 7) == 0x58
    host.write("CMD", 6, 0xA0A0)
    assert [a for a, _ in host.writes] == [address_bits("CMD", 4), address_bits("CMD", 6)]
    assert [v for _, v in host.writes] == [0x1212, 0xA0A0]


def test_iordy_never_driven(program):
    """IORDY stays an input: the program has no side-set at all."""
    host = Gayle(program)
    host.data = [1, 2, 3]
    for _ in range(3):
        host.read("CMD", 0)
    assert program.sideset_bits == 0
