"""The test ISO for the CD-ROM build (docs/atapi.md, slice C2): a folder of
generated files, made into a data ISO (ISO 9660 with Joliet names), whose
every file can be checked after the disc is mounted from the drive.

The files' contents come from their names alone, so nothing large is checked
in and the check needs only the mounted folder.

Build one (macOS: hdiutil; elsewhere xorriso, genisoimage or mkisofs):

    cd host && .venv/bin/python -m hddproto.test_iso build /Volumes/CARD/HDDSYNTH.ISO

then serve it (`cdrom = HDDSYNTH.ISO`, the default) and run
`PIO_ENV=bb48_cdrom scripts/rig.sh -m cdrom`. `check MOUNT_DIR` checks a
mounted copy by hand.
"""
from __future__ import annotations

import hashlib
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

VOLUME = "HDDSYNTH_TEST"

# Path -> size. Sizes around the 2,048-byte frame and the 512-byte DRQ
# block, empty and one-byte files, a few that span many reads, a nested
# folder, and Joliet long names beside 8.3 ones.
FILES: dict[str, int] = {
    "README.TXT": 0,  # replaced by a text below
    "EMPTY.BIN": 0,
    "ONE.BIN": 1,
    "BLOCK511.BIN": 511,
    "BLOCK512.BIN": 512,
    "FRAME.BIN": 2048,
    "FRAME1.BIN": 2049,
    "FRAMES3.BIN": 3 * 2048 - 1,
    "64K.BIN": 65536,
    "BIG1MB.BIN": 1 << 20,
    "BIG5MB.BIN": 5 << 20,
    "DATA/SUB/DEEP.BIN": 12345,
    "DATA/SUB/DEEPER/LAST.BIN": 4096,
    "A long file name with spaces.bin": 7000,
}

README = (b"HDDSynthPro test disc: every file's contents follow from its name\r\n"
          b"(host/hddproto/test_iso.py), so a copy read from the drive can be checked.\r\n")


def contents(path: str, size: int) -> bytes:
    if path == "README.TXT":
        return README
    return random.Random(f"hddsynth:{path}").randbytes(size)


def expected() -> dict[str, bytes]:
    return {path: contents(path, size) for path, size in FILES.items()}


def make_folder(root: Path) -> None:
    for path, data in expected().items():
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def build(out: Path) -> Path:
    """Writes the test ISO to `out`; returns it."""
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / VOLUME
        make_folder(folder)
        out = Path(out)
        if out.exists():
            out.unlink()
        made = Path(tmp) / "made.iso"  # hdiutil adds ".iso" to any other name (HDDSYNTH.ISO.iso)
        if sys.platform == "darwin":
            cmd = ["hdiutil", "makehybrid", "-iso", "-joliet", "-default-volume-name", VOLUME,
                   "-o", str(made), str(folder)]
        else:
            tool = shutil.which("xorriso") or shutil.which("genisoimage") or shutil.which("mkisofs")
            if not tool:
                raise RuntimeError("no ISO maker: install xorriso (or genisoimage)")
            if tool.endswith("xorriso"):
                cmd = [tool, "-as", "mkisofs", "-J", "-V", VOLUME, "-o", str(made), str(folder)]
            else:
                cmd = [tool, "-J", "-V", VOLUME, "-o", str(made), str(folder)]
        subprocess.run(cmd, check=True, capture_output=True)
        shutil.move(str(made), str(out))
    return out


def check(mount: Path) -> list[str]:
    """The problems with a mounted copy: missing files, wrong sizes or data.
    Empty means every file matched."""
    problems = []
    for path, want in expected().items():
        f = Path(mount) / path
        if not f.exists():
            problems.append(f"{path}: missing")
            continue
        got = f.read_bytes()
        if got != want:
            at = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))
            problems.append(f"{path}: {len(got)} bytes (want {len(want)}), first difference at byte {at}")
    return problems


def digest() -> str:
    """One SHA-256 over every expected file, for a log line."""
    h = hashlib.sha256()
    for path, data in sorted(expected().items()):
        h.update(path.encode() + b"\0" + data)
    return h.hexdigest()[:16]


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[0] == "build":
        out = build(Path(argv[1]))
        print(f"{out}: {out.stat().st_size} bytes, {out.stat().st_size // 2048} frames, volume {VOLUME}, "
              f"files {digest()}")
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
