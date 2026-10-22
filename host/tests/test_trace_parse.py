"""The Python trace parser against lines in the firmware's format."""
import pytest

from hddproto import trace

pytestmark = pytest.mark.sim

LINES = [
    "READY HDDSynthPro 0.1.0 slice0",
    "RESET 100 ASSERT",
    "RESET 130 RELEASE",
    "EV 2000 WR CTL 6 DEVCTRL 0004 x1 0 05bf0004",
    "EV 2100 RD CMD 7 STATUS 0080 x5000 3000 05770080",
    "EV 9000 WR CMD 6 DEVICE 00a0 x1 0 05f600a0",
    "STALL 9500",
    "DROP 12",
]


def test_parse_kinds():
    t = trace.parse(LINES)
    assert t.resets == [(100, "ASSERT"), (130, "RELEASE")]
    assert len(t.accesses) == 3
    status = t.accesses[1]
    assert (status.op, status.block, status.reg, status.data, status.count) == \
        ("RD", "CMD", "STATUS", 0x80, 5000)
    assert t.total_accesses() == 5002
    assert t.stalls == [9500]
    assert t.dropped == 12
    assert t.other == ["READY HDDSynthPro 0.1.0 slice0"]
    assert [a.reg for a in t.soft_resets()] == ["DEVCTRL"]


def test_summary_mentions_every_register():
    text = trace.summary(trace.parse(LINES))
    for reg in ("DEVCTRL", "STATUS", "DEVICE"):
        assert reg in text


def test_op_of_raw_samples():
    idle = (1 << 22) | (1 << 23)
    assert trace.op_of(idle) == "NONE"
    assert trace.op_of(idle & ~(1 << 22)) == "RD"
    assert trace.op_of(idle & ~(1 << 23)) == "WR"
    assert trace.op_of(0) == "BOTH"
