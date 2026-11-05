#!/usr/bin/env python3
"""Makes a BIN/CUE from an ISO, to test the CD-ROM drive's CUE sheets
(docs/atapi.md): each 2,048-byte frame becomes a raw 2,352-byte MODE1 frame
(sync, header with its address, the data, then 288 bytes of EDC/ECC left as
zeros: the drive serves only the data, so it never reads them; other tools
that check EDC/ECC will reject the BIN). Optionally a silent audio track
after it, as a mixed-mode game's music would be, with a 2-second pregap, so
the drive must stop the data track where the audio starts.

    python tools/iso_to_bincue.py DOOM_SE.ISO                 # DOOM_SE.BIN, DOOM_SE.CUE
    python tools/iso_to_bincue.py DOOM_SE.ISO --audio 30      # plus 30 s of silence as track 2
    python tools/iso_to_bincue.py DOOM_SE.ISO --mode2         # MODE2/2352 (XA form 1)

Copy both files to the card's root folder, then `cdrom = DOOM_SE.CUE` (or
console M DOOM_SE.CUE). Reading the drive's disc back should give the ISO's
bytes exactly. Standard library only.
"""
import argparse
import os
import sys

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"
FRAMES_PER_SECOND = 75
PREGAP = 2 * FRAMES_PER_SECOND


def bcd(n: int) -> int:
    return (n // 10) << 4 | n % 10


def msf(frame: int) -> str:
    return f"{frame // (60 * 75):02d}:{frame // 75 % 60:02d}:{frame % 75:02d}"


def header(frame: int, mode: int) -> bytes:
    address = frame + PREGAP  # a disc's first frame is at 00:02:00
    return bytes([bcd(address // (60 * 75)), bcd(address // 75 % 60), bcd(address % 75), mode])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("--audio", type=int, default=0, metavar="SECONDS", help="add a silent audio track")
    ap.add_argument("--mode2", action="store_true", help="MODE2/2352 (XA form 1) instead of MODE1/2352")
    ap.add_argument("--out", help="the BIN/CUE's name without extension (default: the ISO's)")
    a = ap.parse_args()

    size = os.path.getsize(a.iso)
    if size % 2048:
        sys.exit(f"{a.iso}: {size} bytes isn't a whole number of 2,048-byte frames")
    base = a.out or os.path.splitext(a.iso)[0]
    bin_path, cue_path = base + ".BIN", base + ".CUE"
    frames = size // 2048
    with open(a.iso, "rb") as iso, open(bin_path, "wb") as out:
        for f in range(frames):
            data = iso.read(2048)
            if a.mode2:
                sub = bytes([0, 0, 0x08, 0]) * 2  # file 0, channel 0, submode: data, coding 0; twice
                out.write(SYNC + header(f, 2) + sub + data + bytes(280))
            else:
                out.write(SYNC + header(f, 1) + data + bytes(288))
        silence = bytes(2352)
        for _ in range((PREGAP + a.audio * FRAMES_PER_SECOND) if a.audio else 0):
            out.write(silence)
    name = os.path.basename(bin_path)
    lines = [f'FILE "{name}" BINARY', f"  TRACK 01 {'MODE2' if a.mode2 else 'MODE1'}/2352", "    INDEX 01 00:00:00"]
    if a.audio:
        lines += ["  TRACK 02 AUDIO", f"    INDEX 00 {msf(frames)}", f"    INDEX 01 {msf(frames + PREGAP)}"]
    with open(cue_path, "w", newline="\r\n") as cue:
        cue.write("\n".join(lines) + "\n")
    print(f"{bin_path}: {frames} data frames{f', then {a.audio} s of audio' if a.audio else ''}\n{cue_path}:")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
