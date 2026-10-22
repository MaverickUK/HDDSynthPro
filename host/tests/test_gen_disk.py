"""The Python test-disk generator against the known answers that
test/native/test_main.cpp checks the C++ generator against."""
import zlib

import pytest

from hddproto import gen_disk

pytestmark = pytest.mark.sim

KNOWN_CRC32 = {0: 0xE1249ABB, 1: 0xE9018A87, 12345: 0xD29A1FF2, 131071: 0x69680A43, 0x0FFFFFFF: 0x907ED2FE}


@pytest.mark.parametrize("lba,crc", KNOWN_CRC32.items())
def test_known_answers(lba, crc):
    assert zlib.crc32(gen_disk.sector(lba)) == crc


def test_header_names_the_sector():
    s = gen_disk.sector(12345)
    assert int.from_bytes(s[0:4], "little") == 12345
    assert int.from_bytes(s[4:8], "little") == ~12345 & 0xFFFFFFFF


def test_first_mismatch_finds_a_swapped_sector():
    data = bytearray(gen_disk.sectors(10, 4))
    assert gen_disk.first_mismatch(bytes(data), 10) is None
    data[1024:1536] = gen_disk.sector(99)  # LBA 12 replaced by LBA 99
    assert gen_disk.first_mismatch(bytes(data), 10) == (12, 0)
    data = bytearray(gen_disk.sectors(10, 1))
    data[300] ^= 1
    assert gen_disk.first_mismatch(bytes(data), 10) == (10, 300)
