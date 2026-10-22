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
    "CMD 10000 ec IDENTIFY 0 0",
    "DONE 10400 ec 50 00 400",
    "CMD 11000 20 READ_SECTORS 131071 1",
    "DONE 11300 20 51 10 300",
    "CMD 12000 20 READ_SECTORS 0 256",
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


def test_parse_commands():
    t = trace.parse(LINES)
    assert [c.name for c in t.commands] == ["IDENTIFY", "READ_SECTORS", "READ_SECTORS"]
    ident, bad, pending = t.commands
    assert ident.ok and ident.duration_us == 400
    assert (bad.lba, bad.count, bad.status, bad.error) == (131071, 1, 0x51, 0x10)
    assert not bad.ok
    assert pending.status is None and not pending.ok
    assert "IDENTIFY x1" in trace.summary(t)


def test_activity_lines_and_time_in_states():
    t = trace.parse([
        "MARK 1000",
        "ACT 2000 SEQ 0 0",
        "ACT 5000 RAND 80 40000",
        "ACT 9000 IDLE 0 0",
        "MARK 10000",
    ])
    assert t.marks == [1000, 10000]
    assert [a.state for a in t.activity] == ["SEQ", "RAND", "IDLE"]
    assert t.activity[1].avg_seek == 40000
    spent = trace.time_in_states(t.activity, 1000, 10000)
    assert spent == {"IDLE": 1000 + 1000, "SEQ": 3000, "RAND": 4000}
    # A window starting mid-state takes the state in force at its start.
    assert trace.time_in_states(t.activity, 6000, 8000) == {"IDLE": 0, "SEQ": 0, "RAND": 2000}
