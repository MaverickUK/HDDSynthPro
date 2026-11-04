"""ATAPI exchanges rebuilt from a raw bus trace (EV lines): the listen build's
recording of a real CD-ROM drive (docs/atapi.md, slice C0), or the drive
build's own with `t` on. Each ATA command, and for PACKET (A0h) the 12-byte
packet, the bytes the host read back and how it ended, so what a host asks a
real drive can be held against what the CD-ROM build answers.

    cd host && .venv/bin/python -m hddproto.packets TRACE.txt

prints each exchange, then the opcodes the host used that src/mmc.h doesn't
handle (they'd get ILLEGAL REQUEST from the emulator).
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from hddproto import trace
from hddproto.paths import REPO

# Packet opcodes by name: the ones src/mmc.h handles and others hosts send.
NAMES = {
    0x00: "TEST_UNIT_READY", 0x03: "REQUEST_SENSE", 0x12: "INQUIRY", 0x15: "MODE_SELECT_6",
    0x1A: "MODE_SENSE_6", 0x1B: "START_STOP", 0x1E: "PREVENT_ALLOW", 0x23: "READ_FORMAT_CAPACITIES",
    0x25: "READ_CAPACITY", 0x28: "READ_10", 0x2B: "SEEK_10", 0x2F: "VERIFY_10", 0x42: "READ_SUBCHANNEL",
    0x43: "READ_TOC", 0x44: "READ_HEADER", 0x45: "PLAY_AUDIO_10", 0x46: "GET_CONFIG", 0x47: "PLAY_AUDIO_MSF",
    0x4A: "GET_EVENT", 0x4B: "PAUSE_RESUME", 0x4E: "STOP_PLAY_SCAN", 0x51: "READ_DISC_INFO",
    0x52: "READ_TRACK_INFO", 0x55: "MODE_SELECT_10", 0x5A: "MODE_SENSE_10", 0xA4: "REPORT_KEY",
    0xA8: "READ_12", 0xAD: "READ_DVD_STRUCTURE", 0xB9: "READ_CD_MSF", 0xBB: "SET_CD_SPEED",
    0xBD: "MECHANISM_STATUS", 0xBE: "READ_CD",
}


def handled_opcodes(mmc_h: Path | None = None) -> set[int]:
    """The opcodes the CD-ROM build handles: src/mmc.h's OP_ constants."""
    text = (mmc_h or REPO / "src" / "mmc.h").read_text()
    return {int(v, 16) for v in re.findall(r"constexpr uint8_t OP_\w+ = 0x([0-9A-Fa-f]+);", text)}


@dataclass
class Exchange:
    t_us: int
    device: int          # 0 or 1, from the last DEVICE write
    command: int         # the ATA command
    packet: bytes = b""  # PACKET only
    limit: int = 0       # PACKET: the byte count limit the host set
    data_words: int = 0  # words the host read from DATA before the next command
    blocks: list[int] = field(default_factory=list)  # byte counts the host read (LBAMID/LBAHIGH), per block
    status: int | None = None  # the last Status the host read
    error: int | None = None   # the last Error it read

    @property
    def opcode(self) -> int | None:
        return self.packet[0] if self.command == 0xA0 and self.packet else None

    @property
    def name(self) -> str:
        if self.command == 0xA0:
            return NAMES.get(self.opcode, f"?{self.opcode:02x}") if self.packet else "PACKET(incomplete)"
        return trace_names.get(self.command, f"ATA_{self.command:02x}")

    def __str__(self):
        dev = f"dev{self.device}"
        st = "" if self.status is None else f" status={self.status:02x}"
        err = "" if self.error is None or self.status is None or not self.status & 1 else f" error={self.error:02x}"
        if self.command == 0xA0:
            blocks = f" blocks={self.blocks}" if self.blocks else ""
            return (f"{self.t_us:>12} {dev} A0 {self.name:<22} [{self.packet.hex(' ')}] limit={self.limit} "
                    f"words={self.data_words}{blocks}{st}{err}")
        return f"{self.t_us:>12} {dev} {self.command:02X} {self.name:<22} words={self.data_words}{st}{err}"


trace_names = {0x08: "DEVICE_RESET", 0x90: "EXEC_DIAGNOSTIC", 0xA1: "IDENTIFY_PACKET", 0xEC: "IDENTIFY",
               0xEF: "SET_FEATURES", 0xE5: "CHECK_POWER", 0xE0: "STANDBY_IMMED", 0xE1: "IDLE_IMMED",
               0x20: "READ_SECTORS", 0xC6: "SET_MULTIPLE"}


def exchanges(t: trace.Trace) -> list[Exchange]:
    out: list[Exchange] = []
    regs = {"FEATURES": 0, "LBAMID": 0, "LBAHIGH": 0, "DEVICE": 0}
    current: Exchange | None = None
    packet = bytearray()
    lba_mid_read: int | None = None
    for a in t.accesses:
        if a.block != "CMD" and not (a.block == "CTL" and a.reg == "ALTSTATUS"):
            continue
        for _ in range(a.count):
            if a.op == "WR":
                if a.reg == "COMMAND":
                    current = Exchange(a.t_us, 1 if regs["DEVICE"] & 0x10 else 0, a.data & 0xFF,
                                       limit=regs["LBAMID"] | regs["LBAHIGH"] << 8)
                    out.append(current)
                    packet = bytearray()
                elif a.reg == "DATA" and current and current.command == 0xA0 and len(packet) < 12:
                    packet += bytes((a.data & 0xFF, a.data >> 8))
                    if len(packet) == 12:
                        current.packet = bytes(packet)
                elif a.reg in regs:
                    regs[a.reg] = a.data & 0xFF
            elif a.op == "RD" and current:
                if a.reg == "DATA":
                    current.data_words += 1
                elif a.reg == "STATUS" or a.reg == "ALTSTATUS":
                    current.status = a.data & 0xFF
                elif a.reg == "ERROR":
                    current.error = a.data & 0xFF
                elif a.reg == "LBAMID":
                    lba_mid_read = a.data & 0xFF
                elif a.reg == "LBAHIGH" and lba_mid_read is not None and current.packet:
                    current.blocks.append(lba_mid_read | (a.data & 0xFF) << 8)
                    lba_mid_read = None
    return out


def report(lines: list[str]) -> str:
    t = trace.parse(lines)
    xs = exchanges(t)
    handled = handled_opcodes()
    out = [str(x) for x in xs]
    used = sorted({x.opcode for x in xs if x.opcode is not None})
    missing = [op for op in used if op not in handled]
    out.append("")
    out.append(f"{len(xs)} commands, {sum(1 for x in xs if x.command == 0xA0)} packets; opcodes used: "
               + (", ".join(f"{op:02x} {NAMES.get(op, '?')}" for op in used) or "none"))
    out.append("not handled by the CD-ROM build: "
               + (", ".join(f"{op:02x} {NAMES.get(op, '?')}" for op in missing) or "none"))
    if t.dropped:
        out.append(f"WARNING: {t.dropped} events were dropped; exchanges may be incomplete")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__)
        return 2
    print(report(Path(argv[0]).read_text(errors="replace").splitlines()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
