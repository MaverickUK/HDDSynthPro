"""Checks a folder drive's image (test/tools/make_folder_img) against the
directory it was made from, through pyfatfs, an independent FAT reader:
the partition table, then every folder and file, in listing order, byte for
byte. Short names differ from the long ones, so entries are matched by
position: the tool lists each folder sorted by long name, and pyfatfs returns
its subfolders first, then its files, each in that order.

Usage: python scripts/check_folder_img.py <directory> <image>
Needs: pip install pyfatfs"""
import hashlib
import os
import struct
import sys

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS


def listing(path):
    """A folder's entries in the order pyfatfs lists the image's: folders, then
    files (PyFatFS.listdir returns dirs + files), each sorted by name."""
    names = sorted(n for n in os.listdir(path) if not n.startswith("."))
    entries = [(n, os.path.isdir(os.path.join(path, n))) for n in names]
    return [e for e in entries if e[1]] + [e for e in entries if not e[1]]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main(directory, image):
    with open(image, "rb") as f:
        mbr = f.read(512)
    assert mbr[510:512] == b"\x55\xaa", "no MBR signature"
    ptype = mbr[0x1BE + 4]
    start, count = struct.unpack_from("<II", mbr, 0x1BE + 8)
    assert ptype == 0x06, f"partition type {ptype:02x}, not FAT16 (06h)"
    size = os.path.getsize(image)
    assert (start + count) * 512 <= size, "the partition runs past the disk"

    pf = PyFat(offset=start * 512)
    pf.open(image, read_only=True)
    assert pf.fat_type == 16, f"FAT{pf.fat_type}, not FAT16"
    fs = PyFatFS(image, offset=start * 512, read_only=True)

    checked = [0, 0]  # files, folders

    def walk(host_dir, image_dir):
        theirs = [n for n in fs.listdir(image_dir) if n not in (".", "..")]
        ours = listing(host_dir)
        assert len(theirs) == len(ours), f"{image_dir}: {len(theirs)} entries, {len(ours)} expected"
        # pyfatfs: folders first, then files, each in directory order (the tool's sorted order)
        for (name, is_dir), short in zip(ours, theirs):
            host_path = os.path.join(host_dir, name)
            image_path = image_dir.rstrip("/") + "/" + short
            assert fs.isdir(image_path) == is_dir, f"{image_path}: folder/file mismatch with {name}"
            if is_dir:
                checked[1] += 1
                walk(host_path, image_path)
            else:
                with open(host_path, "rb") as h:
                    want = h.read()
                with fs.openbin(image_path) as g:
                    got = g.read()
                assert len(got) == len(want), f"{image_path}: {len(got)} bytes, {len(want)} expected"
                assert digest(got) == digest(want), f"{image_path}: content differs from {name}"
                checked[0] += 1

    walk(directory, "/")
    fs.close()
    print(f"ok: FAT16, partition at {start} ({count} sectors), {checked[0]} files and {checked[1]} folders "
          f"identical to {directory}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
