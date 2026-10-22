"""A cycle-level simulator for the subset of PIO used by src/ide_listen.pio.

It runs the real pioasm output (hex) so tests check the program that is
actually built. Modelled: one instruction per cycle, the two-cycle input
synchroniser, OUT shifting right, a joined 8-deep RX FIFO and blocking PUSH
with the RXSTALL flag. Not modelled: delays, side-set, IRQ, autopush/pull.
Anything else raises NotImplementedError rather than guessing.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

CLOCK_HZ = 150_000_000
CYCLE_PS = round(1e12 / CLOCK_HZ)  # 6667 ps
SYNC_CYCLES = 2  # input synchroniser delay
MASK32 = 0xFFFFFFFF


def find_pioasm() -> str | None:
    """pioasm from the PlatformIO packages, or PATH, or $PIOASM."""
    if os.environ.get("PIOASM"):
        return os.environ["PIOASM"]
    candidate = Path.home() / ".platformio/packages/tool-pioasm-rp2040-earlephilhower/pioasm"
    if candidate.exists():
        return str(candidate)
    return shutil.which("pioasm")


def assemble(pio_file: Path, pioasm: str) -> list[int]:
    out = subprocess.run([pioasm, "-o", "hex", str(pio_file)], check=True,
                         capture_output=True, text=True).stdout
    return [int(line, 16) for line in out.split()]


@dataclass
class PioSim:
    program: list[int]
    wrap_target: int = 0
    wrap: int | None = None
    fifo_depth: int = 8
    x: int = 0
    y: int = 0
    isr: int = 0
    osr: int = 0
    pc: int = 0
    rx_fifo: list[int] = field(default_factory=list)
    rx_stall: bool = False

    def __post_init__(self):
        if self.wrap is None:
            self.wrap = len(self.program) - 1

    def _advance(self):
        self.pc = self.wrap_target if self.pc == self.wrap else self.pc + 1

    def step(self, pins: int):
        """Executes one instruction; `pins` is what the synchroniser presents."""
        instr = self.program[self.pc]
        opcode = instr >> 13
        if (instr >> 8) & 0x1F:
            raise NotImplementedError("delay/side-set")
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
            else:
                raise NotImplementedError(f"jmp condition {cond}")
            if take:
                self.pc = addr
            else:
                self._advance()
        elif opcode == 0b011:  # OUT, shifting right
            dest, count = (instr >> 5) & 7, (instr & 0x1F) or 32
            value = self.osr & ((1 << count) - 1)
            self.osr = (self.osr >> count) if count < 32 else 0
            if dest == 1:
                self.x = value
            elif dest == 2:
                self.y = value
            elif dest != 3:
                raise NotImplementedError(f"out destination {dest}")
            self._advance()
        elif opcode == 0b101:  # MOV
            dest, op, src = (instr >> 5) & 7, (instr >> 3) & 3, instr & 7
            value = {0: pins, 1: self.x, 2: self.y, 3: 0, 6: self.isr, 7: self.osr}.get(src)
            if value is None:
                raise NotImplementedError(f"mov source {src}")
            if op == 1:
                value = ~value & MASK32
            elif op != 0:
                raise NotImplementedError(f"mov op {op}")
            if dest == 1:
                self.x = value
            elif dest == 2:
                self.y = value
            elif dest == 6:
                self.isr = value
            elif dest == 7:
                self.osr = value
            else:
                raise NotImplementedError(f"mov destination {dest}")
            self._advance()
        elif opcode == 0b100 and not (instr >> 7) & 1:  # PUSH
            if (instr >> 6) & 1:
                raise NotImplementedError("push iffull")
            block = (instr >> 5) & 1
            if len(self.rx_fifo) >= self.fifo_depth:
                if block:
                    self.rx_stall = True
                    return  # stall: retry next cycle
            else:
                self.rx_fifo.append(self.isr)
            self.isr = 0
            self._advance()
        else:
            raise NotImplementedError(f"opcode {opcode:03b}")


def run(sim: PioSim, wave, duration_ps: int, drain_every_cycles: int | None = 1) -> list[int]:
    """Runs `sim` against `wave(t_ps) -> 32-bit sample` and returns every
    word that reached the FIFO, in order. The consumer takes one word every
    `drain_every_cycles` (None: never)."""
    words: list[int] = []
    cycles = duration_ps // CYCLE_PS
    for k in range(cycles):
        t = (k - SYNC_CYCLES) * CYCLE_PS
        sim.step(wave(t) if t >= 0 else wave(0))
        if drain_every_cycles and k % drain_every_cycles == 0 and sim.rx_fifo:
            words.append(sim.rx_fifo.pop(0))
    if drain_every_cycles:
        words.extend(sim.rx_fifo)
        sim.rx_fifo.clear()
    return words
