"""What the CD-ROM drive behind the USB-IDE adapter is, and which of the
commands the recording session needs reach it. Run before a session.

    python probe.py                 # list CD drives; probe the only one
    python probe.py --drive E:      # this one
    python probe.py --tray          # also open and close the tray

Administrator terminal; a data CD in the drive (as full as you have: seeks
reach as far out as its data does).
"""
from __future__ import annotations

import argparse
import sys
import time

import scsi
from drive_ops import Ops


def pick_drive(arg: str | None) -> str:
    from scsi_win import list_cd_drives
    drives = list_cd_drives()
    for path, letter, name in drives:
        print(f"  {letter}  {name}")
    if arg:
        return arg
    if len(drives) == 1:
        return drives[0][0]
    sys.exit("give --drive (none found)" if not drives else "more than one CD drive: give --drive")


def check(label: str, result, extra: str = "") -> bool:
    mark = "ok  " if result.ok else ("no  " if result.sense and result.sense.unsupported else "FAIL")
    print(f"  {mark} {label:28} {result.seconds * 1000:8.0f} ms  {'' if result.ok else result}{extra}")
    return result.ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drive", help="\\\\.\\CdRomN or a drive letter")
    ap.add_argument("--tray", action="store_true", help="open and close the tray too")
    a = ap.parse_args()

    from scsi_win import CdDrive
    print("CD drives:")
    path = pick_drive(a.drive)
    with CdDrive(path) as drive:
        ops = Ops(drive)
        d = ops.info()
        print(f"\n{path}: {d.name or '?'}")
        print(f"  disc: {d.sectors:,} sectors ({d.sectors * scsi.SECTOR / 1e6:.0f} MB), data out to "
              f"{ops.max_radius():.1f} mm of 58")
        print(f"  fastest {d.max_x:.0f}x, mechanism: {d.tray}")
        for note in d.notes:
            print(f"  note: {note}")
        if not d.sectors:
            sys.exit("no data disc: put one in and run again")

        print("\nCommands (the session's):")
        results = {}
        results["speed_max"] = check("SET CD SPEED fastest", ops.set_speed(None))
        results["speed_4x"] = check("SET CD SPEED 4x", ops.set_speed(4))
        ops.set_speed(None)
        if results["speed_4x"]:
            obeyed = ops.speed_obeyed()
            results["speed_4x"] = obeyed
            print(f"       {'the drive reads slower at 4x' if obeyed else 'but the drive reads at its own speed whatever is asked'}")
        results["read"] = check("READ(10) 32 sectors", ops.read_at(1000, 32))
        results["seek_in"] = check("seek to 26 mm (READ 1)", ops.seek_radius(26))
        results["seek_out"] = check(f"seek to {ops.max_radius():.0f} mm (READ 1)", ops.seek_radius(ops.max_radius()))
        results["seek10"] = check("SEEK(10)", drive.command(scsi.seek10(5000)))
        results["stop"] = check("START STOP: spin down", ops.spin_down())
        time.sleep(3)
        r = ops.spin_up()
        ready = ops.wait_ready(30)
        results["start"] = check("START STOP: spin up", r, f"  (ready after {ready:.1f} s more)" if ready >= 0 else "")
        if a.tray:
            results["open"] = check("START STOP: open tray", ops.open_tray())
            time.sleep(2)
            r = ops.close_tray()
            ready = ops.wait_ready(40)
            results["close"] = check("START STOP: close tray", r, f"  (disc ready {ready:.1f} s later)")

        print()
        if not results["speed_4x"]:
            print("The read speed can't be set (refused, or accepted and ignored): the session records the drive at "
                  "its own speed only, and there's no 1x sound for CD audio.")
        if not (results["stop"] and results["start"]):
            print("Spin up/down don't reach the drive: the session can't record them on command; the drive's own "
                  "idle spin-down (a minute or more untouched) still can be.")
        if results["read"] and results["seek_out"]:
            print("Reads and seeks work: the session can run.")
        else:
            sys.exit("reads fail: nothing to record yet")


if __name__ == "__main__":
    main()
