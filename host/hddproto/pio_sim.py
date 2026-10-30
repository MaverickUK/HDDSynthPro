"""A cycle-level simulator for the subset of PIO used by src/*.pio.

It runs the real pioasm output, so tests check the program that is actually
built. Modelled:
  - one instruction per cycle, plus delay cycles;
  - the two-cycle input synchroniser, bypassable per pin;
  - side-set (optional, to pin values or directions), applied when an
    instruction issues, even if it then stalls;
  - OUT and IN shifting right, PUSH/PULL with blocking, and joined or 4-deep FIFOs;
  - WAIT on gpio/pin/jmppin, JMP conditions including pin;
  - MOV from STATUS, as the RX FIFO level test (EXECCTRL STATUS_SEL = RX);
  - IRQ set and clear (not wait), into `irq_flags`.
Anything else raises NotImplementedError rather than guessing.

Pins use the sample layout of src/bus_sample.h throughout: bit n is GPIO
(4 + n), the program's IN_BASE. Pin indices passed in (out base, side-set
base, jmp pin) are in that layout too.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

CLOCK_HZ = 200_000_000
CYCLE_PS = round(1e12 / CLOCK_HZ)  # 5000 ps
SYNC_CYCLES = 2  # input synchroniser delay
MASK32 = 0xFFFFFFFF
GPIO_OFFSET = 4  # sample bit n = GPIO (n + 4)


def find_pioasm() -> str | None:
    """pioasm from the PlatformIO packages, or $PIOASM, or PATH."""
    if os.environ.get("PIOASM"):
        return os.environ["PIOASM"]
    for exe in ("pioasm", "pioasm.exe"):
        candidate = Path.home() / ".platformio/packages/tool-pioasm-rp2040-earlephilhower" / exe
        if candidate.exists():
            return str(candidate)
    return shutil.which("pioasm")


@dataclass
class Program:
    instructions: list[int]
    wrap_target: int
    wrap: int
    sideset_bits: int = 0      # including the enable bit when optional
    sideset_optional: bool = False
    sideset_pindirs: bool = False
    offsets: dict[str, int] = field(default_factory=dict)  # public labels


def assemble(pio_file: Path, pioasm: str, program: str | None = None) -> Program:
    """Assembles a .pio file and reads back one program's configuration: the
    only one, or the one named `program`."""
    out = subprocess.run([pioasm, "-o", "c-sdk", str(pio_file)], check=True,
                         capture_output=True, text=True).stdout
    if program is not None:
        # pioasm's output has a section per program, each opening with a
        # banner comment holding its name.
        sections = re.split(r"^// -+ //\n// (\w+) //\n// -+ //$", out, flags=re.M)
        named = dict(zip(sections[1::2], sections[2::2]))
        if program not in named:
            raise ValueError(f"no program {program!r} in {pio_file.name} (has {sorted(named)})")
        out = named[program]
    instructions = [int(h, 16) for h in re.findall(r"^\s+0x([0-9a-f]{4}), //", out, re.M)]
    wrap_target = int(re.search(r"_wrap_target (\d+)", out).group(1))
    wrap = int(re.search(r"_wrap (\d+)", out).group(1))
    program = Program(instructions, wrap_target, wrap)
    program.offsets = {name: int(n) for name, n in re.findall(r"#define \w+?_offset_(\w+) (\d+)u", out)}
    m = re.search(r"sm_config_set_sideset\(&c, (\d+), (true|false), (true|false)\)", out)
    if m:
        program.sideset_bits = int(m.group(1))
        program.sideset_optional = m.group(2) == "true"
        program.sideset_pindirs = m.group(3) == "true"
    return program


@dataclass
class PioSim:
    program: Program | list[int]
    out_base: int = 0
    out_count: int = 32
    sideset_base: int = 0
    jmp_pin: int = 0
    fifo_depth: int = 8          # 8 = RX (or TX) joined; the slice 1 program uses 4 each way
    tx_depth: int = 4
    bypass_mask: int = 0         # pins that skip the input synchroniser
    autopush: bool = False       # push when 32 bits have been shifted in (threshold 32 only)
    in_shift_left: bool = False  # IN shifts left (new bits at the bottom)
    set_base: int = 0            # SET pins/pindirs mapping
    set_count: int = 1
    # STATUS (for MOV): all ones while the RX FIFO holds fewer than this many
    # words, else 0. None: the program isn't expected to read STATUS.
    status_rx_below: int | None = None
    x: int = 0
    y: int = 0
    isr: int = 0
    isr_count: int = 0           # bits shifted into ISR since it was last emptied
    osr: int = 0
    pc: int = 0
    pindirs: int = 0             # 1 = output
    pin_out: int = 0             # output values
    rx_fifo: list[int] = field(default_factory=list)
    tx_fifo: list[int] = field(default_factory=list)
    rx_stall: bool = False
    irq_flags: int = 0           # IRQ 0-7, as IRQ SET/CLEAR leave them
    _delay: int = 0

    def __post_init__(self):
        if isinstance(self.program, list):
            self.program = Program(self.program, 0, len(self.program) - 1)

    # -- helpers --------------------------------------------------------------
    def _advance(self):
        p = self.program
        self.pc = p.wrap_target if self.pc == p.wrap else self.pc + 1

    def _set_bits(self, current: int, base: int, count: int, value: int) -> int:
        mask = ((1 << count) - 1) << base
        return (current & ~mask & MASK32) | ((value << base) & mask)

    def _side_set_and_delay(self, instr: int) -> int:
        """Applies side-set; returns the delay count."""
        p = self.program
        field_ = (instr >> 8) & 0x1F
        bits = p.sideset_bits
        delay = field_ & ((1 << (5 - bits)) - 1) if bits < 5 else 0
        if bits:
            side = field_ >> (5 - bits)
            enabled = True
            value_bits = bits
            if p.sideset_optional:
                enabled = bool(side >> (bits - 1))
                value_bits = bits - 1
                side &= (1 << value_bits) - 1
            if enabled:
                if p.sideset_pindirs:
                    self.pindirs = self._set_bits(self.pindirs, self.sideset_base, value_bits, side)
                else:
                    self.pin_out = self._set_bits(self.pin_out, self.sideset_base, value_bits, side)
        return delay

    # -- execution ------------------------------------------------------------
    def step(self, pins: int):
        """One cycle. `pins` is what the input synchroniser presents."""
        if self._delay:
            self._delay -= 1
            return
        instr = self.program.instructions[self.pc]
        delay = self._side_set_and_delay(instr)
        if self._execute(instr, pins):  # completed (not stalled)
            self._delay = delay

    def _execute(self, instr: int, pins: int) -> bool:
        opcode = instr >> 13
        if opcode == 0b000:  # JMP
            cond, addr = (instr >> 5) & 7, instr & 0x1F
            if cond == 0:
                take = True
            elif cond == 1:
                take = self.x == 0
            elif cond == 2:
                take = self.x != 0
                self.x = (self.x - 1) & MASK32
            elif cond == 3:
                take = self.y == 0
            elif cond == 4:
                take = self.y != 0
                self.y = (self.y - 1) & MASK32
            elif cond == 5:
                take = self.x != self.y
            elif cond == 6:
                take = bool((pins >> self.jmp_pin) & 1)
            else:
                raise NotImplementedError(f"jmp condition {cond}")
            if take:
                self.pc = addr
            else:
                self._advance()
            return True

        if opcode == 0b001:  # WAIT
            polarity, source, index = (instr >> 7) & 1, (instr >> 5) & 3, instr & 0x1F
            if source == 0:      # absolute GPIO
                bit = index - GPIO_OFFSET
            elif source == 1:    # relative to IN_BASE (bit 0)
                bit = index
            elif source == 3:    # relative to JMP_PIN (PIO v1)
                bit = self.jmp_pin + index
            else:
                raise NotImplementedError("wait irq")
            if ((pins >> bit) & 1) != polarity:
                return False  # stall
            self._advance()
            return True

        if opcode == 0b010:  # IN, shifting right: new bits enter at the top
            src, count = (instr >> 5) & 7, (instr & 0x1F) or 32
            value = {0: pins, 1: self.x, 2: self.y, 3: 0, 6: self.isr, 7: self.osr}.get(src)
            if value is None:
                raise NotImplementedError(f"in source {src}")
            value &= (1 << count) - 1
            full = self.autopush and self.isr_count + count >= 32
            if full and len(self.rx_fifo) >= self.fifo_depth:
                self.rx_stall = True
                return False  # stalls, as `push block` does, until there's room
            if count == 32:
                self.isr = value
            elif self.in_shift_left:
                self.isr = ((self.isr << count) | value) & MASK32
            else:
                self.isr = ((self.isr >> count) | (value << (32 - count))) & MASK32
            self.isr_count = min(32, self.isr_count + count)
            if full:
                self.rx_fifo.append(self.isr)
                self.isr, self.isr_count = 0, 0
            self._advance()
            return True

        if opcode == 0b011:  # OUT, shifting right
            dest, count = (instr >> 5) & 7, (instr & 0x1F) or 32
            value = self.osr & ((1 << count) - 1)
            self.osr = (self.osr >> count) if count < 32 else 0
            if dest == 0:
                self.pin_out = self._set_bits(self.pin_out, self.out_base, min(count, self.out_count), value)
            elif dest == 1:
                self.x = value
            elif dest == 2:
                self.y = value
            elif dest == 4:
                self.pindirs = self._set_bits(self.pindirs, self.out_base, min(count, self.out_count), value)
            elif dest != 3:
                raise NotImplementedError(f"out destination {dest}")
            self._advance()
            return True

        if opcode == 0b101:  # MOV
            dest, op, src = (instr >> 5) & 7, (instr >> 3) & 3, instr & 7
            if src == 5:  # STATUS
                if self.status_rx_below is None:
                    raise NotImplementedError("mov from STATUS without status_rx_below")
                value = MASK32 if len(self.rx_fifo) < self.status_rx_below else 0
            else:
                value = {0: pins, 1: self.x, 2: self.y, 3: 0, 6: self.isr, 7: self.osr}.get(src)
            if value is None:
                raise NotImplementedError(f"mov source {src}")
            if op == 1:
                value = ~value & MASK32
            elif op != 0:
                raise NotImplementedError(f"mov op {op}")
            if dest == 0:
                self.pin_out = self._set_bits(self.pin_out, self.out_base, self.out_count, value)
            elif dest == 1:
                self.x = value
            elif dest == 2:
                self.y = value
            elif dest == 3:  # PINDIRS (PIO v1), through the OUT mapping
                self.pindirs = self._set_bits(self.pindirs, self.out_base, self.out_count, value)
            elif dest == 6:
                self.isr, self.isr_count = value, 0
            elif dest == 7:
                self.osr = value
            else:
                raise NotImplementedError(f"mov destination {dest}")
            self._advance()
            return True

        if opcode == 0b100:
            is_pull = (instr >> 7) & 1
            if (instr >> 6) & 1:
                raise NotImplementedError("push iffull / pull ifempty")
            block = (instr >> 5) & 1
            if is_pull:
                if self.tx_fifo:
                    self.osr = self.tx_fifo.pop(0)
                elif block:
                    return False  # stall until core 1 answers
                else:
                    self.osr = self.x
            else:
                if len(self.rx_fifo) >= self.fifo_depth:
                    if block:
                        self.rx_stall = True
                        return False
                else:
                    self.rx_fifo.append(self.isr)
                self.isr, self.isr_count = 0, 0
            self._advance()
            return True

        if opcode == 0b110:  # IRQ
            clear, wait, index = (instr >> 6) & 1, (instr >> 5) & 1, instr & 7
            if wait or instr & 0x18:
                raise NotImplementedError("irq wait, or a relative/other-block index")
            if clear:
                self.irq_flags &= ~(1 << index)
            else:
                self.irq_flags |= 1 << index
            self._advance()
            return True

        if opcode == 0b111:  # SET (X, Y, pindirs)
            dest, data = (instr >> 5) & 7, instr & 0x1F
            if dest == 1:
                self.x = data
            elif dest == 2:
                self.y = data
            elif dest == 4:
                self.pindirs = self._set_bits(self.pindirs, self.set_base, self.set_count, data)
            else:
                raise NotImplementedError(f"set destination {dest}")
            self._advance()
            return True

        raise NotImplementedError(f"opcode {opcode:03b}")


def synced(history: list[int], k: int, bypass_mask: int) -> int:
    """The pins PIO sees at cycle k: two cycles late, except bypassed pins."""
    late = history[k - SYNC_CYCLES] if k >= SYNC_CYCLES else history[0]
    return (late & ~bypass_mask & MASK32) | (history[k] & bypass_mask)


def run(sim: PioSim, wave, duration_ps: int, drain_every_cycles: int | None = 1) -> list[int]:
    """Runs `sim` against a fixed waveform `wave(t_ps) -> 32-bit sample`
    and returns every word that reached the RX FIFO, in order. The consumer
    takes one word every `drain_every_cycles` (None: never)."""
    words: list[int] = []
    history: list[int] = []
    for k in range(duration_ps // CYCLE_PS):
        history.append(wave(k * CYCLE_PS))
        sim.step(synced(history, k, sim.bypass_mask))
        if drain_every_cycles and k % drain_every_cycles == 0 and sim.rx_fifo:
            words.append(sim.rx_fifo.pop(0))
    if drain_every_cycles:
        words.extend(sim.rx_fifo)
        sim.rx_fifo.clear()
    return words
