"""Copies a disk or an image into a split image that a FAT32 card can hold:
NAME.IMG, then NAME.I01, NAME.I02... of at most 4,095 MiB each (FAT32's file
limit is 4 GiB less a byte; parts must be whole sectors). The drive serves
the parts as one disk when HDDSYNTH.INI says `device0 = NAME.IMG`
(src/image_parts.h).

Usage: python3 tools/split_image.py SOURCE DEST_FOLDER NAME.IMG
  SOURCE        an image file, or a raw disk such as /dev/rdisk5 (macOS; needs
                `sudo chown $USER /dev/rdiskN` first)
  DEST_FOLDER   the card, e.g. /Volumes/HDDSYNTH
  NAME.IMG      the first part's name; the others follow it

Each part is checked against the source as it's written (SHA-256), and the
whole copy against the source at the end.
"""
import hashlib
import os
import sys

PART_BYTES = 4095 * 1024 * 1024  # under FAT32's 4 GiB limit, a whole number of sectors
CHUNK = 1024 * 1024


def part_name(base: str, n: int) -> str:
    """As image_parts::partName: NAME.IMG, NAME.I01, NAME.I02..."""
    if n == 0:
        return base
    stem = base.rsplit(".", 1)[0] if "." in base else base
    return f"{stem}.I{n:02d}"


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source, dest, base = sys.argv[1:]
    for n in range(8):
        if os.path.exists(os.path.join(dest, part_name(base, n))):
            sys.exit(f"{part_name(base, n)} already exists in {dest}: left as it is")
    whole = hashlib.sha256()
    total = 0
    with open(source, "rb", buffering=0) as src:
        n = 0
        while True:
            path = os.path.join(dest, part_name(base, n))
            written = 0
            part_hash = hashlib.sha256()
            with open(path, "wb") as out:
                while written < PART_BYTES:
                    data = src.read(min(CHUNK, PART_BYTES - written))
                    if not data:
                        break
                    out.write(data)
                    part_hash.update(data)
                    whole.update(data)
                    written += len(data)
                out.flush()
                os.fsync(out.fileno())
            if written == 0:
                os.remove(path)
                break
            with open(path, "rb") as check:  # what reached the card
                again = hashlib.sha256()
                while chunk := check.read(CHUNK):
                    again.update(chunk)
            if again.digest() != part_hash.digest():
                sys.exit(f"{path}: doesn't match the source")
            print(f"{part_name(base, n)}: {written} bytes, checked")
            total += written
            n += 1
            if written < PART_BYTES:
                break
    print(f"{total} bytes in {n} part(s); source SHA-256 {whole.hexdigest()}")


if __name__ == "__main__":
    main()
