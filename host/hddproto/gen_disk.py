"""The slice 3 test disk, computed from the LBA: the Python twin of
src/gen_disk.h. Both are checked against the same known answers."""
from __future__ import annotations

SECTOR_BYTES = 512
DEFAULT_SECTORS = 131072  # 64 MB
_M32 = 0xFFFFFFFF


def sector(lba: int) -> bytes:
    out = bytearray(SECTOR_BYTES)
    out[0:4] = (lba & _M32).to_bytes(4, "little")
    out[4:8] = (~lba & _M32).to_bytes(4, "little")
    state = (lba * 2654435761 + 0x12345678) & _M32
    for i in range(8, SECTOR_BYTES):
        state = (state * 1664525 + 1013904223) & _M32
        out[i] = state >> 24
    return bytes(out)


def sectors(lba: int, count: int) -> bytes:
    return b"".join(sector(lba + i) for i in range(count))


def first_mismatch(data: bytes, lba: int) -> tuple[int, int] | None:
    """(lba, byte offset) of the first byte that differs, or None."""
    for i in range(len(data) // SECTOR_BYTES):
        want = sector(lba + i)
        got = data[i * SECTOR_BYTES:(i + 1) * SECTOR_BYTES]
        if got != want:
            off = next(j for j in range(SECTOR_BYTES) if got[j] != want[j])
            return lba + i, off
    return None
