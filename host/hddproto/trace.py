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
class Command:
    """A command from CMD/DONE lines (drive builds)."""
    t_us: int
    command: int
    name: str
    lba: int
    count: int
    status: int | None = None   # None: not completed (yet)
    error: int | None = None
    duration_us: int | None = None

    @property
    def ok(self) -> bool:
        return self.status is not None and not self.status & 0x01

    def __str__(self):
        end = "" if self.status is None else f" -> st={self.status:02x} err={self.error:02x} {self.duration_us} us"
        return f"{self.name}({self.command:02x}) lba={self.lba} count={self.count}{end}"


@dataclass
class Activity:
    """An ACT line: the activity classifier changed state (drive builds)."""
    t_us: int
    state: str        # IDLE, SEQ or RAND
    seek_pct: int     # share of seeks among the window's commands
    avg_seek: int     # their mean distance, sectors


@dataclass
class Trace:
    accesses: list[Access] = field(default_factory=list)
    resets: list[tuple[int, str]] = field(default_factory=list)  # (t_us, ASSERT|RELEASE)
    stalls: list[int] = field(default_factory=list)
    dropped: int = 0
    commands: list[Command] = field(default_factory=list)
    activity: list[Activity] = field(default_factory=list)
    marks: list[int] = field(default_factory=list)  # MARK timestamps, us
    # Reads IORDY held past ATA's limit, as (t_us, register, ns).
    slow_reads: list[tuple[int, str, int]] = field(default_factory=list)
    other: list[str] = field(default_factory=list)  # anything else the drive printed

    @property
    def empty(self) -> bool:
        return not self.accesses and not self.resets and not self.commands

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
            if kind == "ACT" and len(parts) == 5:
                t.activity.append(Activity(int(parts[1]), parts[2], int(parts[3]), int(parts[4])))
                continue
            if kind == "MARK" and len(parts) == 2:
                t.marks.append(int(parts[1]))
                continue
            if kind == "SLOWREAD" and len(parts) == 4:
                t.slow_reads.append((int(parts[1]), parts[2], int(parts[3])))
                continue
            if kind == "CMD" and len(parts) == 6:
                t.commands.append(Command(int(parts[1]), int(parts[2], 16), parts[3],
                                          int(parts[4]), int(parts[5])))
                continue
            if kind == "DONE" and len(parts) == 6:
                command = int(parts[2], 16)
                for c in reversed(t.commands):
                    if c.command == command and c.status is None:
                        c.status, c.error, c.duration_us = int(parts[3], 16), int(parts[4], 16), int(parts[5])
                        break
                continue
        except ValueError:
            pass
        t.other.append(line)
    return t


def opening_sequence(lines: list[str], length: int = 40) -> list[str]:
    """The first `length` steps of a trace, in the order printed, for comparing
    one probe with another: reset edges and accesses, with repeats of a step
    merged (an adapter may poll a register a different number of times) and
    read data left out (with no drive answering, the bus floats)."""
    steps: list[str] = []
    for line in lines:
        t = parse([line])
        if t.resets:
            step = f"RESET {t.resets[0][1]}"
        elif t.accesses:
            a = t.accesses[0]
            step = f"{a.op} {a.block}.{a.reg}" + (f" {a.data:04x}" if a.op == "WR" else "")
        else:
            continue
        if not steps or steps[-1] != step:
            steps.append(step)
            if len(steps) == length:
                break
    return steps


def summary(t: Trace, sequence_lines: int = 60) -> str:
    """A readable report: totals, a histogram per register and the opening sequence."""
    out = [f"{len(t.accesses)} trace lines, {t.total_accesses()} accesses, {len(t.commands)} commands, "
           f"{len(t.resets)} reset edges, {len(t.stalls)} stalls, {t.dropped} dropped"]
    hist = Counter()
    for a in t.accesses:
        hist[(a.op, a.block, a.reg)] += a.count
    out.append("By register:")
    for (op, block, reg), n in sorted(hist.items(), key=lambda kv: -kv[1]):
        out.append(f"  {op:4} {block}.{reg:9} {n}")
    if t.commands:
        names = Counter(c.name for c in t.commands)
        out.append("Commands: " + ", ".join(f"{n} x{k}" for n, k in names.most_common()))
    out.append(f"Sequence (first {sequence_lines}):")
    events = ([(a.t_us, str(a)) for a in t.accesses] + [(ts, f"RESET {e}") for ts, e in t.resets]
              + [(c.t_us, str(c)) for c in t.commands])
    events.sort(key=lambda e: e[0])
    start = events[0][0] if events else 0
    for ts, text in events[:sequence_lines]:
        out.append(f"  +{(ts - start) & 0xFFFFFFFF:>10} us  {text}")
    return "\n".join(out)


def time_in_states(activity: list[Activity], start_us: int, end_us: int) -> dict[str, int]:
    """Microseconds spent in each activity state between two drive times,
    from the ACT transitions (the state before the first one is IDLE)."""
    spent = {"IDLE": 0, "SEQ": 0, "RAND": 0}
    state, since = "IDLE", start_us
    for a in sorted(activity, key=lambda a: a.t_us):
        if a.t_us <= start_us:
            state = a.state
            continue
        if a.t_us >= end_us:
            break
        spent[state] += a.t_us - since
        state, since = a.state, a.t_us
    spent[state] += max(0, end_us - since)
    return spent
