"""The activity link's sentences (docs/activity-link.md), as a receiver reads
them: the reference for the HDD Synth's parser, and for tests.

    $HDA,1,<busy>,<class>,<op>*<checksum>\\r\\n

A receiver ignores any line that doesn't start with $HDA, fails its checksum
or is longer than 80 bytes, and ignores fields after the ones it knows, so a
later version can add some at the end. The firmware's side is
src/activity_link.*; change both together.

Version 2 adds head targets, one per media command (at most every 2 ms):

    $HDE,2,<pos>,<op>,<sectors>*<checksum>\\r\\n

pos is the command's first LBA as a fraction of the disk, 0-65535.
"""
from __future__ import annotations

from dataclasses import dataclass
from functools import reduce

MAX_LINE = 80
CLASSES = {"I": "idle", "S": "sequential", "R": "random"}
OPS = {"R": "read", "W": "write", "-": None}


@dataclass(frozen=True)
class Activity:
    version: int
    busy: bool
    kind: str          # "idle", "sequential" or "random"
    op: str | None     # "read", "write" or None (no media command yet)


def checksum(body: str) -> str:
    """The NMEA-style checksum: XOR of every byte between $ and *, two upper-case hex digits."""
    return f"{reduce(lambda acc, c: acc ^ ord(c), body, 0):02X}"


def format_sentence(busy: bool, kind: str, op: str | None) -> str:
    """The sentence the drive sends for these fields, with \\r\\n."""
    cls = {v: k for k, v in CLASSES.items()}[kind]
    o = {v: k for k, v in OPS.items()}[op]
    body = f"HDA,1,{int(busy)},{cls},{o}"
    return f"${body}*{checksum(body)}\r\n"


def parse(line: str | bytes) -> Activity | None:
    """One received line (with or without its line ending); None if a
    receiver must ignore it."""
    if isinstance(line, bytes):
        try:
            line = line.decode("ascii")
        except UnicodeDecodeError:
            return None
    line = line.rstrip("\r\n")
    if len(line) > MAX_LINE or not line.startswith("$HDA,"):
        return None
    body, star, given = line[1:].rpartition("*")
    if not star or given.upper() != checksum(body) or len(given) != 2:
        return None
    fields = body.split(",")
    if len(fields) < 5 or not fields[1].isdigit():
        return None
    busy, cls, op = fields[2:5]
    if busy not in ("0", "1") or cls not in CLASSES or op not in OPS:
        return None
    return Activity(int(fields[1]), busy == "1", CLASSES[cls], OPS[op])


@dataclass(frozen=True)
class HeadTarget:
    version: int
    pos: int           # 0 (LBA 0) to 65535 (the last LBA)
    op: str | None     # "read", "write" or None (a verify or seek)
    sectors: int


def head_position(lba: int, total_sectors: int) -> int:
    """lba * 65535 / (total_sectors - 1), as the drive works it out."""
    if total_sectors < 2:
        return 0
    return min(65535, lba * 65535 // (total_sectors - 1))


def format_head(pos: int, op: str | None, sectors: int) -> str:
    """The $HDE the drive sends, with \\r\\n."""
    o = {v: k for k, v in OPS.items()}[op]
    body = f"HDE,2,{pos},{o},{sectors}"
    return f"${body}*{checksum(body)}\r\n"


def parse_head(line: str | bytes) -> HeadTarget | None:
    """One received $HDE line; None if a receiver must ignore it."""
    if isinstance(line, bytes):
        try:
            line = line.decode("ascii")
        except UnicodeDecodeError:
            return None
    line = line.rstrip("\r\n")
    if len(line) > MAX_LINE or not line.startswith("$HDE,"):
        return None
    body, star, given = line[1:].rpartition("*")
    if not star or given.upper() != checksum(body) or len(given) != 2:
        return None
    fields = body.split(",")
    if len(fields) < 5 or not fields[1].isdigit() or int(fields[1]) < 2:
        return None
    pos, op, sectors = fields[2:5]
    if not pos.isdigit() or int(pos) > 65535 or op not in OPS or not sectors.isdigit():
        return None
    return HeadTarget(int(fields[1]), int(pos), OPS[op], int(sectors))
