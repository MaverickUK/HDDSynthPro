"""DriveLog's quiet detection: a CD-ROM's routine polls don't count as activity."""
import pytest

from hddproto.drive_log import PollFilter

pytestmark = pytest.mark.sim


def activity(lines):
    f = PollFilter()
    return [f.is_activity(line) for line in lines]


def test_get_event_polls_are_not_activity():
    # Seen on Windows with `device1 = cdrom`: one poll a second (10 Nov).
    assert activity([
        "CMD 16107540 a0 GET_EVENT 0 0 d1",
        "DONE 16107558 a0 40 00 18",
        "CMD 16107688 a0 TEST_UNIT_READY 0 0 d1",
        "DONE 16107704 a0 41 54 16",
    ]) == [False, False, False, False]


def test_other_commands_are_activity():
    assert activity([
        "CMD 13439454 20 READ_SECTORS 0 1",
        "DONE 13450001 20 50 00 10547",
        "CMD 16060783 a0 INQUIRY 0 0 d1",
        "DONE 16060801 a0 40 00 18",
        "RESET 20136386 ASSERT",
    ]) == [True, True, True, True, True]


def test_a_packet_after_a_poll_is_activity():
    # The DONE of a READ after a poll's DONE counts, though both are PACKET.
    assert activity([
        "CMD 1 a0 GET_EVENT 0 0 d1",
        "DONE 2 a0 40 00 18",
        "CMD 3 a0 READ_10 16 1 d1",
        "DONE 4 a0 40 00 900",
    ]) == [False, False, True, True]
