"""The activity link's sentences (docs/activity-link.md), as a receiver reads
them: the reference for the HDD Synth's parser, and for tests.

    $HDA,1,<busy>,<class>,<op>*<checksum>\\r\\n

A receiver ignores any line that doesn't start with $HDA, fails its checksum
or is longer than 80 bytes, and ignores fields after the ones it knows, so a
later version can add some at the end. The firmware's side is
src/activity_link.*; change both together.
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
