"""ATAPI exchanges rebuilt from raw EV lines (hddproto/packets.py)."""
from __future__ import annotations

import pytest

from hddproto import packets, trace

pytestmark = pytest.mark.sim


def ev(t, op, reg, data, count=1, block="CMD", addr=0):
    return f"EV {t} {op} {block} {addr} {reg} {data:04x} x{count} 0 00000000"


# A host selects device 0, sets a 36-byte limit, sends INQUIRY (its zero
# words merged into one line, as the trace does), reads the byte count and
# 18 words, then the status; then READ DISC INFORMATION, refused.
LINES = [
    ev(10, "WR", "DEVICE", 0xA0, addr=6),
    ev(11, "WR", "FEATURES", 0x00, addr=1),
    ev(12, "WR", "LBAMID", 0x24, addr=4),
    ev(13, "WR", "LBAHIGH", 0x00, addr=5),
    ev(14, "WR", "COMMAND", 0xA0, addr=7),
    ev(15, "RD", "ALTSTATUS", 0x58, block="CTL", addr=6),
    ev(16, "WR", "DATA", 0x0012),
    ev(17, "WR", "DATA", 0x0000),
    ev(18, "WR", "DATA", 0x0024),
    ev(19, "WR", "DATA", 0x0000, count=3),
    ev(20, "RD", "STATUS", 0x58, addr=7),
    ev(21, "RD", "LBAMID", 0x24, addr=4),
    ev(22, "RD", "LBAHIGH", 0x00, addr=5),
    ev(23, "RD", "DATA", 0x8005, count=18),
    ev(30, "RD", "STATUS", 0x50, addr=7),
    ev(40, "WR", "COMMAND", 0xA0, addr=7),
    ev(41, "WR", "DATA", 0x0051),
    ev(42, "WR", "DATA", 0x0000, count=5),
    ev(43, "RD", "STATUS", 0x51, addr=7),
    ev(44, "RD", "ERROR", 0x54, addr=1),
    ev(50, "WR", "COMMAND", 0xA1, addr=7),
    ev(51, "RD", "DATA", 0x85C0, count=256),
]


def test_exchanges():
    xs = packets.exchanges(trace.parse(LINES))
    inquiry, disc_info, identify = xs
    assert inquiry.packet == bytes([0x12, 0, 0, 0, 0x24, 0, 0, 0, 0, 0, 0, 0])
    assert (inquiry.name, inquiry.limit, inquiry.data_words, inquiry.blocks, inquiry.status) == \
        ("INQUIRY", 36, 18, [36], 0x50)
    assert disc_info.name == "READ_DISC_INFO" and disc_info.status == 0x51 and disc_info.error == 0x54
    assert (identify.command, identify.name, identify.data_words) == (0xA1, "IDENTIFY_PACKET", 256)


def test_report_flags_what_the_cd_build_doesnt_handle():
    text = packets.report(LINES)
    assert "not handled by the CD-ROM build: 51 READ_DISC_INFO" in text
    assert "12 INQUIRY" in text


def test_handled_opcodes_follow_mmc_h():
    handled = packets.handled_opcodes()
    assert {0x00, 0x03, 0x12, 0x25, 0x28, 0x43, 0x46, 0x4A, 0x5A, 0xA8} <= handled
    assert 0xBE not in handled  # READ CD: not yet
    assert all(op in packets.NAMES for op in handled)
