"""SCSI/MMC commands for a CD-ROM drive, and decoding its answers.

Pure Python (no I/O), so it can be tested without a drive. The transport that
sends a command block (CDB) and moves the data is in scsi_win.py.

The commands are those a CD-ROM drive makes its sounds with (MMC-2 and up,
which any ATAPI CD-ROM drive from the mid-1990s on understands, through a
USB-IDE adapter too):

    START STOP UNIT   spin up, spin down, open and close the tray
    SET CD SPEED      how fast the disc turns while reading
    READ(10)          a seek to the LBA, then the data
    SEEK(10)          a seek alone (some drives do nothing for it)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

SECTOR = 2048  # a data CD's sector (Mode 1)

# ---- Command blocks ------------------------------------------------------------


def test_unit_ready() -> bytes:
    return bytes(6)


def inquiry(length: int = 96) -> bytes:
    return bytes([0x12, 0, 0, 0, length, 0])


def request_sense(length: int = 18) -> bytes:
    return bytes([0x03, 0, 0, 0, length, 0])


def read_capacity() -> bytes:
    return bytes([0x25]) + bytes(9)


def start_stop(start: bool, load_eject: bool = False, immediate: bool = False) -> bytes:
    """START STOP UNIT: start=True spins up (with load_eject: closes the tray
    and loads), start=False spins down (with load_eject: opens the tray).
    `immediate`: the drive answers at once instead of when it's done."""
    return bytes([0x1B, 1 if immediate else 0, 0, 0, (2 if load_eject else 0) | (1 if start else 0), 0])


def set_cd_speed(read_kbs: int = 0xFFFF) -> bytes:
    """SET CD SPEED (BBh): the read speed in kB/s (176 per 1x; 0xFFFF the
    fastest the drive has). The drive rounds it to a speed it can do."""
    read_kbs = min(read_kbs, 0xFFFF)
    return bytes([0xBB, 0]) + struct.pack(">HH", read_kbs, 0xFFFF) + bytes(6)


def set_streaming(read_kbs: int, last_lba: int) -> tuple[bytes, bytes]:
    """SET STREAMING (B6h, MMC-3): the read speed for the whole disc, as DVD-era
    drives take it (some ignore SET CD SPEED). Returns the CDB and its 28-byte
    performance descriptor (sent as data): `read_kbs` kB in each 1,000 ms."""
    descriptor = bytes([0, 0, 0, 0]) + struct.pack(">IIIIII", 0, last_lba, read_kbs, 1000, read_kbs, 1000)
    cdb = bytes([0xB6]) + bytes(8) + struct.pack(">H", len(descriptor)) + bytes([0])
    return cdb, descriptor


def read10(lba: int, count: int) -> bytes:
    return bytes([0x28, 0]) + struct.pack(">I", lba) + bytes([0]) + struct.pack(">H", count) + bytes([0])


def seek10(lba: int) -> bytes:
    return bytes([0x2B, 0]) + struct.pack(">I", lba) + bytes(4)


def mode_sense10(page: int, length: int = 64) -> bytes:
    """MODE SENSE(10), current values, block descriptors off."""
    return bytes([0x5A, 0x08, page & 0x3F, 0, 0, 0, 0]) + struct.pack(">H", length) + bytes([0])


def prevent_allow(prevent: bool) -> bytes:
    return bytes([0x1E, 0, 0, 0, 1 if prevent else 0, 0])


# ---- Answers -------------------------------------------------------------------

KX = 176.4  # kB/s per "x" (75 sectors of 2,352 bytes a second); drives report 176


@dataclass
class Inquiry:
    device_type: int
    removable: bool
    vendor: str
    product: str
    revision: str

    @property
    def name(self) -> str:
        return f"{self.vendor} {self.product} {self.revision}".strip()


def parse_inquiry(data: bytes) -> Inquiry:
    text = lambda b: b.decode("ascii", "replace").strip()  # noqa: E731
    return Inquiry(data[0] & 0x1F, bool(data[1] & 0x80), text(data[8:16]), text(data[16:32]), text(data[32:36]))


def parse_capacity(data: bytes) -> tuple[int, int]:
    """READ CAPACITY: (sectors, bytes per sector). The last LBA is sectors - 1."""
    last, size = struct.unpack(">II", data[:8])
    return last + 1, size


@dataclass
class Capabilities:
    """Mode page 2Ah (CD capabilities), the parts that matter for its sounds."""
    max_read_kbs: int
    current_read_kbs: int
    can_eject: bool
    tray: str  # "caddy", "tray", "pop-up", "changer" or "?"
    audio_play: bool

    @property
    def max_x(self) -> float:
        return self.max_read_kbs / KX

    @property
    def current_x(self) -> float:
        return self.current_read_kbs / KX


def parse_capabilities(data: bytes) -> Capabilities | None:
    """From MODE SENSE(10)'s answer: an 8-byte header, then the page."""
    if len(data) < 8:
        return None
    descriptors = struct.unpack(">H", data[6:8])[0]
    page = data[8 + descriptors:]
    if len(page) < 16 or page[0] & 0x3F != 0x2A:
        return None
    mechanism = page[6] >> 5
    tray = {0: "caddy", 1: "tray", 2: "pop-up", 4: "changer", 5: "changer"}.get(mechanism, "?")
    return Capabilities(max_read_kbs=struct.unpack(">H", page[8:10])[0],
                        current_read_kbs=struct.unpack(">H", page[14:16])[0],
                        can_eject=bool(page[6] & 0x08), tray=tray, audio_play=bool(page[4] & 0x01))


@dataclass
class Sense:
    key: int
    asc: int
    ascq: int

    def __str__(self) -> str:
        return f"{self.key:X}/{self.asc:02X}/{self.ascq:02X} {self.meaning}"

    @property
    def meaning(self) -> str:
        known = {
            (0x02, 0x04, 0x01): "becoming ready (spinning up)",
            (0x02, 0x04, 0x00): "not ready",
            (0x02, 0x3A, 0x00): "no disc",
            (0x02, 0x3A, 0x01): "no disc, tray closed",
            (0x02, 0x3A, 0x02): "no disc, tray open",
            (0x06, 0x28, 0x00): "disc changed (unit attention)",
            (0x06, 0x29, 0x00): "reset (unit attention)",
            (0x05, 0x20, 0x00): "command not supported",
            (0x05, 0x24, 0x00): "invalid field in the command",
            (0x05, 0x21, 0x00): "LBA out of range",
            (0x03, 0x11, 0x00): "read error",
        }
        if (self.key, self.asc, self.ascq) in known:
            return known[(self.key, self.asc, self.ascq)]
        return {0: "no sense", 1: "recovered", 2: "not ready", 3: "medium error", 4: "hardware error",
                5: "illegal request", 6: "unit attention", 0xB: "aborted"}.get(self.key, "?")

    @property
    def unsupported(self) -> bool:
        return self.key == 0x05 and self.asc in (0x20, 0x24)


def parse_sense(data: bytes) -> Sense | None:
    """Fixed-format sense data (70h/71h)."""
    if len(data) < 14 or (data[0] & 0x7F) not in (0x70, 0x71):
        return None
    return Sense(data[2] & 0x0F, data[12], data[13])


# ---- Where on the disc ----------------------------------------------------------


def radius_mm(lba: int, sectors_on_disc: int = 333_000) -> float:
    """The laser's radius for `lba` on a CD (constant linear velocity): the
    data starts at 25 mm and a full 74-minute disc (333,000 sectors) ends near
    58 mm; the track's area grows with the sectors, so radius goes as a
    square root."""
    r0, r1 = 25.0, 58.0
    f = max(0.0, min(1.0, lba / sectors_on_disc))
    return (r0 * r0 + f * (r1 * r1 - r0 * r0)) ** 0.5


def lba_at_radius(r_mm: float, sectors_on_disc: int = 333_000) -> int:
    r0, r1 = 25.0, 58.0
    r = max(r0, min(r1, r_mm))
    return int(round((r * r - r0 * r0) / (r1 * r1 - r0 * r0) * sectors_on_disc))


# ---- A command's outcome (any transport) ----------------------------------------


@dataclass
class Result:
    ok: bool
    status: int            # SCSI status: 0 good, 2 check condition
    data: bytes
    sense: Sense | None
    seconds: float         # how long the command took, as the PC saw it
    error: str = ""        # Windows' error, if the pass-through itself failed

    def __str__(self) -> str:
        if self.error:
            return f"failed: {self.error}"
        return "ok" if self.ok else f"status {self.status}, sense {self.sense}"
