"""Cuts a session's recording into an HDD Synth sample pack, by its log.

    python cut.py RECORDING runs/session-<time>.json [--name CD_Mitsumi] [--rate 44100]
                  [--pick spinup=2 --pick access=3] [--offset 4.21]

Finds the two sync beeps at the start (and at the end, to correct for the
recorder's clock), then writes packs/<name>/:

    spinup.wav idle.wav access.wav spindown.wav   what today's HDD Synth plays
    sequential.wav                                its sound for sequential reads (optional in a pack)
    extras/   the rest: tray, seeks by length, reading and spinning at each speed, loading, the
              drive's own spin-down, the room
    takes/    every take of every sound, to choose from (then --pick label=N)
    pack.json where each file came from, and the gain

All at one level (one gain for the whole pack, the loudest peak at -1 dBFS),
so the sounds keep their loudness against each other.
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from array import array
from pathlib import Path

import audio

HERE = Path(__file__).resolve().parent

# The pack's own files: (file, label recorded, from, to, loop), times relative
# to the event's start ("s") or end ("e").
PACK = [
    ("spinup.wav", "spinup", ("s", -0.1), ("e", 0.0), False),
    ("idle.wav", "idle_max", ("s", 1.0), ("e", -1.0), True),
    ("access.wav", "access", ("s", -0.05), ("e", 0.0), False),
    ("spindown.wav", "spindown", ("s", -0.05), ("e", 0.0), False),
    ("sequential.wav", "read_max", ("s", 1.5), ("s", 5.5), True),
]
TAIL_TRIMMED = {"spindown", "auto_spindown"}


def to_audio_time(log_t: float, offset: float, scale: float) -> float:
    return offset + log_t * scale


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording")
    ap.add_argument("log")
    ap.add_argument("--name", help="the pack's folder name (default: CD_ and the drive's model; no spaces)")
    ap.add_argument("--rate", type=int, default=44100, choices=audio.RATES)
    ap.add_argument("--pick", action="append", default=[], help="label=N: use take N of that sound")
    ap.add_argument("--offset", type=float, help="where the first beep starts in the recording, if not found")
    ap.add_argument("--out", default=str(HERE / "packs"))
    a = ap.parse_args()

    log = json.loads(Path(a.log).read_text())
    events = log["events"]
    print(f"reading {a.recording} ...")
    samples = audio.load(a.recording, a.rate)
    rate = a.rate
    print(f"  {len(samples) / rate:.0f} s at {rate} Hz")

    # ---- where the log's time 0 is in the recording
    hz, gap = log["beep_hz"], log["beep_gap_s"]
    start = a.offset if a.offset is not None else audio.find_beeps(samples, rate, hz, gap)
    if start is None:
        sys.exit("no sync beeps found in the first 90 s: give --offset (where the first beep starts, in seconds)")
    scale = 1.0
    end_ev = next((e for e in events if e["label"] == "sync_end"), None)
    if end_ev:
        guess = start + end_ev["start"]
        end = audio.find_beeps(samples, rate, hz, gap, search_from_s=max(0.0, guess - 5), search_s=10)
        if end is not None:
            scale = (end - start) / end_ev["start"]
    drift_ms = (scale - 1) * (end_ev["start"] if end_ev else 0) * 1000
    print(f"  sync: first beep at {start:.3f} s; recorder clock {(scale - 1) * 1e6:+.0f} ppm ({drift_ms:+.0f} ms by the end)"
          + ("" if scale != 1.0 or not end_ev else " (end beeps not found: assumed exact)"))

    def window(ev, frm, to) -> array:
        t0 = ev["start"] if frm[0] == "s" else ev["end"]
        t1 = ev["start"] if to[0] == "s" else ev["end"]
        return audio.cut(samples, rate, to_audio_time(t0 + frm[1], start, scale), to_audio_time(t1 + to[1], start, scale))

    # ---- the room's noise, for trimming spin-downs
    room = next((e for e in events if e["label"] == "room"), None)
    floor = audio.rms(window(room, ("s", 1), ("e", -1))) if room else 30.0
    print(f"  room noise {audio.db(floor):.0f} dBFS")

    # ---- every take
    by_label: dict[str, list[dict]] = {}
    for ev in events:
        if not ev["label"].startswith("sync"):
            by_label.setdefault(ev["label"], []).append(ev)
    picks = dict(p.split("=", 1) for p in a.pick)

    def choose(label: str) -> dict | None:
        takes = by_label.get(label)
        if not takes:
            return None
        if label in picks:
            return next(e for e in takes if e["take"] == int(picks[label]))
        # the median by loudness: not a take with a knock in it, nor one that came out quiet
        levels = sorted(takes, key=lambda e: audio.rms(window(e, ("s", 0), ("e", 0))))
        return levels[len(levels) // 2]

    files: dict[str, array] = {}
    sources: dict[str, str] = {}
    for name, label, frm, to, loop in PACK:
        ev = choose(label)
        if ev is None:
            print(f"  no {label} recorded: {name} left out")
            continue
        s = window(ev, frm, to)
        if label in TAIL_TRIMMED:
            s = audio.trim_tail(s, rate, floor)
        files[name] = audio.make_loop(s, rate) if loop else audio.fade(s, rate, 0.005, 0.03)
        sources[name] = f"{label} take {ev['take']}"

    for label, takes in by_label.items():
        if label in ("room",) or label.startswith("read_") or label.startswith("idle_"):
            ev = takes[0]
            if label.startswith(("read_", "idle_")):
                s = audio.make_loop(window(ev, ("s", 1.0), ("e", -0.5)), rate)
            else:
                s = window(ev, ("s", 0.5), ("e", -0.5))
            files[f"extras/{label}.wav"] = s
            sources[f"extras/{label}.wav"] = f"{label} take {ev['take']}"
        else:
            ev = choose(label)
            post = 0.5 if label == "tray_open" else 0.0
            s = window(ev, ("s", -0.1), ("e", post))
            if label in TAIL_TRIMMED:
                s = audio.trim_tail(s, rate, floor)
            files[f"extras/{label}.wav"] = audio.fade(s, rate, 0.005, 0.03)
            sources[f"extras/{label}.wav"] = f"{label} take {ev['take']}"
        for ev in takes:
            files[f"takes/{label}_{ev['take']}.wav"] = audio.fade(window(ev, ("s", -0.3), ("e", 0.3)), rate)

    # ---- one gain for the pack: its loudest peak at -1 dBFS
    loudest = max(audio.peak(s) for n, s in files.items() if not n.startswith("takes/"))
    gain = (32767 * 10 ** (-1 / 20)) / max(loudest, 1)
    name = a.name or "CD_" + re.sub(r"[^A-Za-z0-9]+", "_", log.get("drive", "drive")).strip("_")[:60]
    out = Path(a.out) / name
    for rel, s in files.items():
        audio.save(out / rel, audio.scale(s, gain), rate)
    meta = {"drive": log.get("drive"), "sectors": log.get("sectors"), "max_x": log.get("max_x"),
            "rate": rate, "gain_db": round(20 * __import__("math").log10(gain), 1),
            "room_noise_dbfs": round(audio.db(floor * gain), 1), "sync_s": round(start, 3),
            "clock_ppm": round((scale - 1) * 1e6), "sources": sources, "session": Path(a.log).name}
    (out / "pack.json").write_text(json.dumps(meta, indent=1))
    print(f"\n{out}: {len([n for n in files if not n.startswith('takes/')])} files, gain {meta['gain_db']:+.1f} dB")
    for n in sorted(sources):
        secs = len(files[n]) / rate
        print(f"  {n:28} {secs:5.1f} s  {sources[n]}")
    print("\nListen: python preview.py " + str(out))


if __name__ == "__main__":
    main()
