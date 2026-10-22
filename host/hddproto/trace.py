"""Parsing the drive's serial trace (format: src/trace_format.h)."""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass, field

# Sample layout (src/bus_sample.h)
_DIOR = 1 << 22
_DIOW = 1 << 23


def op_of(sample: int) -> str:
    """RD, WR, BOTH or NONE for a raw pin sample."""
    rd, wr = not sample & _DIOR, not sample & _DIOW
    return "BOTH" if rd and wr else "RD" if rd else "WR" if wr else "NONE"


@dataclass
class Access:
    t_us: int
    op: str      # RD, WR, BOTH
    block: str   # CMD, CTL, NONE, BOTH
    addr: int
    reg: str
    data: int
    count: int   # identical accesses merged into this line
    span_us: int
    pins: int

    def __str__(self):
        repeat = f" x{self.count}" if self.count > 1 else ""
        return f"{self.op:4} {self.block}.{self.reg:9} {self.data:04x}{repeat}"


@dataclass
class Trace:
    accesses: list[Access] = field(default_factory=list)
    resets: list[tuple[int, str]] = field(default_factory=list)  # (t_us, ASSERT|RELEASE)
    stalls: list[int] = field(default_factory=list)
    dropped: int = 0
    other: list[str] = field(default_factory=list)  # anything else the drive printed

    @property
    def empty(self) -> bool:
        return not self.accesses and not self.resets

    def total_accesses(self) -> int:
        return sum(a.count for a in self.accesses)

    def soft_resets(self) -> list[Access]:
        """Writes to Device Control with SRST (bit 2) set."""
        return [a for a in self.accesses if a.op == "WR" and a.reg == "DEVCTRL" and a.data & 0x04]


def parse(lines: list[str]) -> Trace:
    t = Trace()
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        kind = parts[0]
        try:
            if kind == "EV" and len(parts) == 10:
                t.accesses.append(Access(
                    t_us=int(parts[1]), op=parts[2], block=parts[3], addr=int(parts[4]),
                    reg=parts[5], data=int(parts[6], 16), count=int(parts[7].lstrip("x")),
                    span_us=int(parts[8]), pins=int(parts[9], 16)))
                continue
            if kind == "RESET" and len(parts) == 3:
                t.resets.append((int(parts[1]), parts[2]))
                continue
            if kind == "STALL":
                t.stalls.append(int(parts[1]))
                continue
            if kind == "DROP":
                t.dropped += int(parts[1])
                continue
        except ValueError:
            pass
        t.other.append(line)
    return t


def summary(t: Trace, sequence_lines: int = 60) -> str:
    """A readable report: totals, a histogram per register and the opening sequence."""
    out = [f"{len(t.accesses)} trace lines, {t.total_accesses()} accesses, "
           f"{len(t.resets)} reset edges, {len(t.stalls)} stalls, {t.dropped} dropped"]
    hist = Counter()
    for a in t.accesses:
        hist[(a.op, a.block, a.reg)] += a.count
    out.append("By register:")
    for (op, block, reg), n in sorted(hist.items(), key=lambda kv: -kv[1]):
        out.append(f"  {op:4} {block}.{reg:9} {n}")
    out.append(f"Sequence (first {sequence_lines}):")
    events = [(a.t_us, str(a)) for a in t.accesses] + [(ts, f"RESET {e}") for ts, e in t.resets]
    events.sort(key=lambda e: e[0])
    start = events[0][0] if events else 0
    for ts, text in events[:sequence_lines]:
        out.append(f"  +{(ts - start) & 0xFFFFFFFF:>10} us  {text}")
    return "\n".join(out)
