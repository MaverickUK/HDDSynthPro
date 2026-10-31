"""Builds DOSBENCH.IMG, HDD Synth Pro's benchmark image: a 128 MB disk that
boots FreeDOS and runs HDDBENCH.COM, the same on every machine (README.md).

Layout (16 heads x 63 sectors x 260 cylinders, what the drive's IDENTIFY
reports for this size, inside every BIOS's CHS limits):
  LBA 0              MBR (mbr.asm), one active FAT16 partition
  LBA 1              HDDBENCH header: the layout, for HDDBENCH.COM
  LBA 63-131039      FAT16: FreeDOS, HDDBENCH.COM, AUTOEXEC.BAT (cylinders 0-129)
  LBA 131040-196559  pattern area: pattern(LBA) in every sector, read only (130-194)
  LBA 196560-261071  scratch area: the write tests (195-258)
  LBA 261072-262079  unused: BIOSes keep the last cylinder for diagnostics and
                     report one fewer (INT 13h function 08h), so it's left alone

Needs nasm and mtools (mformat, mcopy); FreeDOS's kernel and shell are
downloaded once into .cache/ and checked against their SHA-256.

Usage: python3 build_image.py [output path, default build/DOSBENCH.IMG]
"""
import array
import hashlib
import os
import struct
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, ".cache")
SECTOR = 512

HEADS, SPT, CYLINDERS = 16, 63, 260
TRACK_CYL = HEADS * SPT
DISK_SECTORS = CYLINDERS * TRACK_CYL                    # 262,080 (128 MB)
PART_LBA = SPT                                          # cylinder 0, head 1
PART_END_CYL = 129
PART_SECTORS = (PART_END_CYL + 1) * TRACK_CYL - PART_LBA
PATTERN_LBA = (PART_END_CYL + 1) * TRACK_CYL
PATTERN_COUNT = 65 * TRACK_CYL
SCRATCH_LBA = PATTERN_LBA + PATTERN_COUNT
SCRATCH_COUNT = 64 * TRACK_CYL
USABLE_SECTORS = (CYLINDERS - 1) * TRACK_CYL            # the random reads' range: not the last cylinder

FREEDOS = "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/"
PACKAGES = {  # FreeDOS 1.3: kernel 2043, FreeCOM 0.85a
    "kernel.zip": "1efdba830a473891bf4c5c81e1858d86c247a0c2a03f00d69b5f40969c442905",
    "freecom.zip": "25fef0309e94e87b88b634452b780bd2821fb6d94b0d0e149ad97633ea7d6be7",
}
BOOT_SECTOR_SRC = "https://raw.githubusercontent.com/FDOS/kernel/ke2043/boot/"
BOOT_SECTOR_FILES = ("boot.asm",)  # the FAT12/16 boot sector, assembled with -dISFAT16


def fetch(url, path, sha256=None):
    if not os.path.exists(path):
        print(f"downloading {url}")
        urllib.request.urlretrieve(url, path)
    if sha256:
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if digest != sha256:
            sys.exit(f"{path}: SHA-256 {digest}, expected {sha256}")


def run(*args, **kw):
    subprocess.run(args, check=True, **kw)


def pattern_sector(lba, seed=0):
    """What HDDBENCH.COM expects in the sector at `lba` (hddbench.asm says why)."""
    lo, hi = lba & 0xFFFF, lba >> 16
    words = array.array("H", bytes(SECTOR))
    words[0] = lo ^ seed
    words[1] = hi ^ seed
    a, b = (lo + 2 * 0x9E37) & 0xFFFF, (hi + 2) & 0xFFFF
    for i in range(2, 256):
        words[i] = a ^ b ^ seed
        a = (a + 0x9E37) & 0xFFFF
        b = (b + 1) & 0xFFFF
    if sys.byteorder != "little":
        words.byteswap()
    return words.tobytes()


def chs(lba):
    c, rest = divmod(lba, TRACK_CYL)
    h, s = divmod(rest, SPT)
    return bytes([h, ((c >> 2) & 0xC0) | (s + 1), c & 0xFF])


def mbr(code):
    entry = (bytes([0x80]) + chs(PART_LBA) + bytes([0x06]) + chs(PART_LBA + PART_SECTORS - 1)
             + struct.pack("<II", PART_LBA, PART_SECTORS))
    sector = code[:440] + bytes(6) + entry + bytes(48) + b"\x55\xAA"
    assert len(sector) == SECTOR
    return sector


def header():
    h = b"HDDBENCH" + struct.pack("<HIIIII", 1, USABLE_SECTORS, PATTERN_LBA, PATTERN_COUNT,
                                  SCRATCH_LBA, SCRATCH_COUNT)
    return h.ljust(SECTOR, b"\0")


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "DOSBENCH.IMG"))
    build = os.path.join(HERE, "build")
    os.makedirs(CACHE, exist_ok=True)
    os.makedirs(build, exist_ok=True)
    os.makedirs(os.path.dirname(out), exist_ok=True)

    for name, digest in PACKAGES.items():
        fetch(FREEDOS + name, os.path.join(CACHE, name), digest)
    for name in BOOT_SECTOR_FILES:
        fetch(BOOT_SECTOR_SRC + name, os.path.join(CACHE, name))
    with zipfile.ZipFile(os.path.join(CACHE, "kernel.zip")) as z:
        kernel = z.read("BIN/KERNL86.SYS")  # 8086 build: runs on anything
    with zipfile.ZipFile(os.path.join(CACHE, "freecom.zip")) as z:
        command = z.read("BIN/COMMAND.COM")

    boot = os.path.join(build, "fat16.bin")
    run("nasm", "-dISFAT16", "-f", "bin", "-o", boot, os.path.join(CACHE, "boot.asm"), cwd=CACHE)
    mbr_bin = os.path.join(build, "mbr.bin")
    run("nasm", "-f", "bin", "-o", mbr_bin, os.path.join(HERE, "mbr.asm"))
    bench = os.path.join(build, "HDDBENCH.COM")
    run("nasm", "-f", "bin", "-o", bench, os.path.join(HERE, "hddbench.asm"))

    # The FAT16 partition, made apart and copied in
    part = os.path.join(build, "part.img")
    with open(part, "wb") as f:
        f.truncate(PART_SECTORS * SECTOR)
    run("mformat", "-i", part, "-T", str(PART_SECTORS), "-h", str(HEADS), "-s", str(SPT),
        "-H", str(PART_LBA), "-B", boot, "-v", "HDDBENCH", "-N", "48444442", "::")
    files = {
        "KERNEL.SYS": kernel,
        "COMMAND.COM": command,
        "HDDBENCH.COM": open(bench, "rb").read(),
    }
    for name in ("FDCONFIG.SYS", "AUTOEXEC.BAT", "README.TXT"):
        files[name] = open(os.path.join(HERE, "dos", name), "rb").read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    staged = os.path.join(build, "files")
    os.makedirs(staged, exist_ok=True)
    for name, data in files.items():  # KERNEL.SYS first: the boot sector finds it anywhere, DOS likes it first
        path = os.path.join(staged, name)
        open(path, "wb").write(data)
        run("mcopy", "-i", part, "-m", path, "::" + name)

    with open(out, "wb") as f:
        f.write(mbr(open(mbr_bin, "rb").read()))
        f.write(header())
        f.write(bytes((PART_LBA - 2) * SECTOR))
        f.write(open(part, "rb").read())
        assert f.tell() == PATTERN_LBA * SECTOR
        for lba in range(PATTERN_LBA, PATTERN_LBA + PATTERN_COUNT):
            f.write(pattern_sector(lba))
        f.truncate(DISK_SECTORS * SECTOR)  # the scratch area: zeros until the first write test
    print(f"{out}: {DISK_SECTORS} sectors ({DISK_SECTORS * SECTOR // 1000000} MB), "
          f"C/H/S {CYLINDERS}/{HEADS}/{SPT}; FAT16 at {PART_LBA} ({PART_SECTORS} sectors), "
          f"pattern {PATTERN_LBA}+{PATTERN_COUNT}, scratch {SCRATCH_LBA}+{SCRATCH_COUNT}")


if __name__ == "__main__":
    main()
