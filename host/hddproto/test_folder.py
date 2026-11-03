"""The test folder for the two-drives build's folder drive (`device1 = GAMES`,
docs/two-drives.md): files whose contents follow from their names, so a copy
read through the drive can be checked anywhere, plus CHECK.BAT for the retro
PC, which copies every file from D: to C:\\HDDTEST and compares each copy with
D: (`FC /B`, MS-DOS 5 or later).

    cd host && .venv/bin/python -m hddproto.test_folder make /Volumes/CARD/GAMES
    cd host && .venv/bin/python -m hddproto.test_folder check DIR

`check` checks a folder holding the copies (C:\\HDDTEST, mounted on the Mac
from the image afterwards), by their 8.3 names as DOS saw them.
"""
from __future__ import annotations

import random
import sys
from pathlib import Path

# Long name on the card -> the 8.3 name the folder drive gives it, as DOS
# shows it (folder_disk's shortName, Windows' rule). Sizes around the
# 512-byte sector and a 2 KB cluster, empty and one-byte files, two big ones
# for timing COPY, a subfolder two deep, and a long name.
FILES: dict[str, tuple[str, int]] = {
    "README.TXT": ("README.TXT", 0),  # the text below
    "EMPTY.BIN": ("EMPTY.BIN", 0),
    "ONE.BIN": ("ONE.BIN", 1),
    "SECT511.BIN": ("SECT511.BIN", 511),
    "SECT512.BIN": ("SECT512.BIN", 512),
    "SECT513.BIN": ("SECT513.BIN", 513),
    "CLUST.BIN": ("CLUST.BIN", 32768 + 1),
    "BIG1MB.BIN": ("BIG1MB.BIN", 1 << 20),
    "BIG2MB.BIN": ("BIG2MB.BIN", (2 << 20) + 12345),
    "Long file name.bin": ("LONGFI~1.BIN", 7000),
    "SUB/INSUB.BIN": ("SUB/INSUB.BIN", 4097),
    "SUB/DEEPER/LAST.BIN": ("SUB/DEEPER/LAST.BIN", 100000),
}

README = (b"HDDSynthPro folder drive test (host/hddproto/test_folder.py).\r\n"
          b"Every file's contents follow from its name, so copies can be checked.\r\n"
          b"Run D:\\CHECK.BAT from C:\\ on the retro PC.\r\n")


def contents(path: str, size: int) -> bytes:
    if path == "README.TXT":
        return README
    return random.Random(f"hddsynth-folder:{path}").randbytes(size)


def dos(path: str) -> str:
    return path.replace("/", "\\")


def check_bat() -> bytes:
    lines = ["@ECHO OFF",
             "REM Copies every file from D: to C:\\HDDTEST and compares each copy with D:.",
             "REM Every FC should say: no differences encountered.",
             "MD C:\\HDDTEST", "MD C:\\HDDTEST\\SUB", "MD C:\\HDDTEST\\SUB\\DEEPER"]
    for long_name, (short, _) in FILES.items():
        lines.append(f"COPY D:\\{dos(short)} C:\\HDDTEST\\{dos(short)} > NUL")
        lines.append(f"FC /B D:\\{dos(short)} C:\\HDDTEST\\{dos(short)}")
    lines.append("ECHO Done. Delete C:\\HDDTEST when its copies have been checked.")
    return ("\r\n".join(lines) + "\r\n").encode()


def make(folder: Path) -> None:
    folder = Path(folder)
    if folder.exists() and any(folder.iterdir()):
        raise SystemExit(f"{folder} already has files in it; not touching them")
    for long_name, (_, size) in FILES.items():
        target = folder / long_name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(contents(long_name, size))
    (folder / "CHECK.BAT").write_bytes(check_bat())


def check(folder: Path) -> list[str]:
    """The problems with a folder of copies, found by their 8.3 names."""
    problems = []
    for long_name, (short, size) in FILES.items():
        want = contents(long_name, size)
        f = next((p for p in (Path(folder) / short, Path(folder) / long_name) if p.exists()), None)
        if f is None:
            problems.append(f"{short}: missing")
            continue
        got = f.read_bytes()
        if got != want:
            at = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))
            problems.append(f"{short}: {len(got)} bytes (want {len(want)}), first difference at byte {at}")
    return problems


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[0] == "make":
        make(Path(argv[1]))
        total = sum(size for _, size in FILES.values())
        print(f"{argv[1]}: {len(FILES)} files and CHECK.BAT, {total / 1e6:.1f} MB")
        return 0
    if len(argv) == 2 and argv[0] == "check":
        problems = check(Path(argv[1]))
        for p in problems:
            print(p)
        print("ok" if not problems else f"{len(problems)} problem(s)")
        return 1 if problems else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
