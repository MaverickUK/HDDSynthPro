"""The folder drive's test folder (hddproto/test_folder.py): it builds, checks
out, catches damage, and CHECK.BAT names files as the folder drive does."""
from __future__ import annotations

import pytest

from hddproto import test_folder

pytestmark = pytest.mark.sim


def test_made_folder_checks_out(tmp_path):
    games = tmp_path / "GAMES"
    test_folder.make(games)
    assert test_folder.check(games) == []
    assert (games / "Long file name.bin").exists()
    assert (games / "SUB" / "DEEPER" / "LAST.BIN").stat().st_size == 100000


def test_damage_is_caught(tmp_path):
    games = tmp_path / "GAMES"
    test_folder.make(games)
    big = games / "BIG2MB.BIN"
    data = bytearray(big.read_bytes())
    data[-1] ^= 0x80
    big.write_bytes(bytes(data))
    (games / "SUB" / "INSUB.BIN").unlink()
    problems = test_folder.check(games)
    assert len(problems) == 2
    assert any(p.startswith("BIG2MB.BIN") and f"byte {len(data) - 1}" in p for p in problems)
    assert "SUB/INSUB.BIN: missing" in problems


def test_copies_by_short_name_check_out(tmp_path):
    # As DOS copies them to C:\HDDTEST: the long name becomes LONGFI~1.BIN
    games, copies = tmp_path / "GAMES", tmp_path / "HDDTEST"
    test_folder.make(games)
    for long_name, (short, _) in test_folder.FILES.items():
        target = copies / short
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((games / long_name).read_bytes())
    assert test_folder.check(copies) == []


def test_check_bat_uses_the_folder_drives_names():
    bat = test_folder.check_bat().decode()
    assert "FC /B D:\\LONGFI~1.BIN C:\\HDDTEST\\LONGFI~1.BIN" in bat
    assert "COPY D:\\SUB\\DEEPER\\LAST.BIN C:\\HDDTEST\\SUB\\DEEPER\\LAST.BIN" in bat
    assert bat.endswith("\r\n")


def test_refuses_a_folder_with_files(tmp_path):
    (tmp_path / "GAMES").mkdir()
    (tmp_path / "GAMES" / "MINE.TXT").write_text("keep")
    with pytest.raises(SystemExit):
        test_folder.make(tmp_path / "GAMES")
    assert (tmp_path / "GAMES" / "MINE.TXT").read_text() == "keep"
