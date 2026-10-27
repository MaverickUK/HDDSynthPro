"""The activity link's sentences: the known answers from docs/activity-link.md
(shared with test/native/test_activity_link.cpp) and the receiver's rules."""
import pytest

from hddproto import activity_link as link

pytestmark = pytest.mark.sim

KNOWN = [
    ("$HDA,1,0,I,-*28", False, "idle", None),
    ("$HDA,1,1,S,R*4C", True, "sequential", "read"),
    ("$HDA,1,1,R,W*48", True, "random", "write"),
    ("$HDA,1,1,S,W*49", True, "sequential", "write"),
    ("$HDA,1,0,R,R*4C", False, "random", "read"),
]


@pytest.mark.parametrize("sentence, busy, kind, op", KNOWN)
def test_known_answers(sentence, busy, kind, op):
    assert link.format_sentence(busy, kind, op) == sentence + "\r\n"
    assert link.parse(sentence + "\r\n") == link.Activity(1, busy, kind, op)
    assert link.parse(sentence.encode()) == link.Activity(1, busy, kind, op)


def test_ignores_what_a_receiver_must_ignore():
    assert link.parse("$HDA,1,1,S,R*4D") is None        # wrong checksum
    assert link.parse("$HDA,1,1,S,R") is None           # no checksum
    assert link.parse("$GPGGA,123519*47") is None        # another sentence
    assert link.parse("HDA,1,1,S,R*4C") is None          # no $
    assert link.parse("$HDA,1,2,S,R*" + link.checksum("HDA,1,2,S,R")) is None  # busy not 0/1
    assert link.parse("$HDA,1,1,X,R*" + link.checksum("HDA,1,1,X,R")) is None  # unknown class
    assert link.parse("$HDA,1,1*" + link.checksum("HDA,1,1")) is None          # too few fields
    long_body = "HDA,1,1,S,R," + "x" * 80
    assert link.parse(f"${long_body}*{link.checksum(long_body)}") is None      # over 80 bytes
    assert link.parse(b"$HDA,1,1,S,R*4C\xff") is None    # not ASCII


def test_accepts_extra_fields_and_lower_case_checksum():
    body = "HDA,2,1,S,W,123,future"
    got = link.parse(f"${body}*{link.checksum(body)}")
    assert got == link.Activity(2, True, "sequential", "write")
    assert link.parse("$HDA,1,1,S,R*4c") == link.Activity(1, True, "sequential", "read")
