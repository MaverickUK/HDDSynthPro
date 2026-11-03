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

With NASM installed, the disc also holds CDCHECK.COM (tools/cdcheck), which
checks every file on DOS: each one's size and CRC-32, under the ISO 9660
names DOS sees, read back from a first build of the disc.
"""
from __future__ import annotations

import hashlib
import random
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

VOLUME = "HDDSYNTH_TEST"
CDCHECK_ASM = Path(__file__).resolve().parents[2] / "tools" / "cdcheck" / "cdcheck.asm"

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


def _make_iso(folder: Path, out: Path) -> None:
    made = out.parent / "made.iso"  # hdiutil adds ".iso" to any other name (HDDSYNTH.ISO.iso)
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


def iso9660_files(iso: bytes) -> dict[str, bytes]:
    """Every file in the ISO 9660 tree (not Joliet's), by the DOS path MSCDEX
    shows ("DATA\\SUB\\DEEP.BIN": no ";1", no trailing dot), with its data."""
    frame = 2048
    pvd = iso[16 * frame:17 * frame]
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise ValueError("no ISO 9660 primary volume descriptor")
    found: dict[str, bytes] = {}

    def walk(extent: int, size: int, prefix: str) -> None:
        data = iso[extent * frame:extent * frame + size]
        at = 0
        while at < len(data):
            length = data[at]
            if length == 0:  # the rest of this frame is padding
                at = (at // frame + 1) * frame
                continue
            rec = data[at:at + length]
            at += length
            name = rec[33:33 + rec[32]]
            if name in (b"\0", b"\1"):
                continue
            start = int.from_bytes(rec[2:6], "little")
            length_bytes = int.from_bytes(rec[10:14], "little")
            text = name.decode("ascii").split(";")[0].rstrip(".")
            if rec[25] & 0x02:
                walk(start, length_bytes, prefix + text + "\\")
            else:
                found[prefix + text] = iso[start * frame:start * frame + length_bytes]

    root = pvd[156:156 + 34]
    walk(int.from_bytes(root[2:6], "little"), int.from_bytes(root[10:14], "little"), "")
    return found


def dos_names(iso: bytes) -> dict[str, str]:
    """Each of FILES' paths -> its ISO 9660 name, matched by contents."""
    on_disc = iso9660_files(iso)
    names = {}
    for path, data in expected().items():
        matches = [n for n, d in on_disc.items() if d == data]
        if len(matches) != 1:
            raise ValueError(f"{path}: {len(matches)} files on the disc have its contents")
        names[path] = matches[0]
    return names


def is_dos_path(name: str) -> bool:
    """Whether DOS can open it: 8.3 names in every part. hdiutil's ISO 9660
    names aren't always (the long name becomes "A LONG FILE NAME WITH SPAC.BIN")."""
    for part in name.split("\\"):
        base, _, ext = part.partition(".")
        if not (1 <= len(base) <= 8 and len(ext) <= 3 and "." not in ext):
            return False
        if not all(c.isalnum() or c in "_-~!#$%&'()@^{}" for c in base + ext):
            return False
    return True


def files_inc(names: dict[str, str]) -> str:
    """CDCHECK's table: each file's DOS path, size and CRC-32, for the files
    DOS can name."""
    lines = [f"skipped_files equ {sum(not is_dos_path(n) for n in names.values())}\n"]
    for path, data in expected().items():
        if not is_dos_path(names[path]):
            continue
        lines.append(f'        db "{names[path]}", 0\n'
                     f"        dd {len(data)}, 0x{zlib.crc32(data):08x}\n")
    return "".join(lines)


def assemble_cdcheck(names: dict[str, str], work: Path) -> bytes:
    work.mkdir(parents=True, exist_ok=True)
    (work / "files.inc").write_text(files_inc(names))
    com = work / "CDCHECK.COM"
    subprocess.run(["nasm", "-f", "bin", "-I", f"{work}/", "-o", str(com), str(CDCHECK_ASM)],
                   check=True, capture_output=True)
    return com.read_bytes()


def build(out: Path) -> Path:
    """Writes the test ISO to `out`; returns it. With NASM, CDCHECK.COM is
    added, built for the names a first build gave the files."""
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / VOLUME
        make_folder(folder)
        out = Path(out)
        if out.exists():
            out.unlink()
        first = Path(tmp) / "first.bin"
        _make_iso(folder, first)
        if shutil.which("nasm"):
            names = dos_names(first.read_bytes())
            (folder / "CDCHECK.COM").write_bytes(assemble_cdcheck(names, Path(tmp) / "cdcheck"))
            _make_iso(folder, first)
            if dos_names(first.read_bytes()) != names:
                raise RuntimeError("the files' ISO 9660 names changed when CDCHECK.COM was added")
        else:
            print("nasm not found: the disc has no CDCHECK.COM", file=sys.stderr)
        shutil.move(str(first), str(out))
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
