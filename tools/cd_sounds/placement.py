"""Where to put the microphone: a 20-second test that compares the drive's
seek and access sounds with its full-speed spinning, so positions can be
tried quickly before a whole session.

    python placement.py --mic "Microphone (Samson Meteor Mic)" [--drive D:] [--label front]

Reports the spinning's level, how far the seeks and accesses stand above it
(their loudest 10 ms against the spinning's typical 10 ms), and the room's
noise. More contrast: the access sounds come through better. Each run is
appended to runs/placement.txt, so positions can be compared.
"""
from __future__ import annotations

import argparse
import random
import statistics
import sys
import time
from pathlib import Path

import audio
import scsi
from drive_ops import Ops
from listen import RATE, Listener, spindle

HERE = Path(__file__).resolve().parent


def levels(samples, start_s: float, end_s: float, window_s: float = 0.01) -> list[float]:
    a, b = int(start_s * RATE), int(end_s * RATE)
    w = int(window_s * RATE)
    return [audio.rms(samples, i, i + w) for i in range(a, b - w, w)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mic", default="Samson Meteor Mic" if sys.platform == "darwin" else "Microphone (Samson Meteor Mic)")
    ap.add_argument("--drive", help="the drive (default: the only one)")
    ap.add_argument("--label", default="", help="a name for this position, for the log")
    a = ap.parse_args()

    import platform_io
    rng = random.Random(5)
    with platform_io.open_drive(platform_io.pick_drive(a.drive)) as d:
        ops = Ops(d)
        ops.info()
        ops.set_speed(None)
        ops.read_for(7, ops.sectors // 3)  # to full speed
        with Listener(a.mic, "placement") as ear:
            t0 = time.perf_counter()
            marks = {}
            time.sleep(5)
            marks["spinning"] = (0.5, 5.0)
            s = time.perf_counter() - t0
            top = ops.max_radius()
            for k in range(4):
                ops.seek_radius(25.5 if k % 2 else top - 0.5)
                time.sleep(1.0)
            marks["seeks"] = (s, time.perf_counter() - t0)
            s = time.perf_counter() - t0
            for _ in range(2):
                for _ in range(6):
                    ops.read_at(scsi.lba_at_radius(rng.uniform(25, top)), 16)
                time.sleep(1.2)
            marks["access"] = (s, time.perf_counter() - t0)
            elapsed = time.perf_counter() - t0
            time.sleep(0.5)

    samples = audio.load(ear.path, RATE)
    lead = max(0.0, len(samples) / RATE - elapsed - 0.8)
    spin = levels(samples, lead + marks["spinning"][0], lead + marks["spinning"][1])
    spin_typical = statistics.median(spin)
    hz, strength = spindle(samples[int((lead + 2) * RATE):int((lead + 2.5) * RATE)])
    out = [f"position {a.label or '-'}: spinning {audio.db(spin_typical):.1f} dBFS (disc {hz} Hz, x{strength:.0f})"]
    for name in ("seeks", "access"):
        ls = sorted(levels(samples, lead + marks[name][0], lead + marks[name][1]))
        loud = ls[int(len(ls) * 0.98)]  # the loudest 2% of 10 ms windows: the clicks
        out.append(f"  {name:7} stand {audio.db(loud) - audio.db(spin_typical):+5.1f} dB above the spinning "
                   f"(loudest {audio.db(loud):.1f} dBFS)")
    report = "\n".join(out)
    print(report)
    with open(HERE / "runs" / "placement.txt", "a") as f:
        f.write(time.strftime("%H:%M:%S ") + report + "\n")


if __name__ == "__main__":
    main()
