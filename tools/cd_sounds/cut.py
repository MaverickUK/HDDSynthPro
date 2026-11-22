"""Cuts one or more sessions' recordings into an HDD Synth sample pack, by
their logs.

    python cut.py runs/session-A.json [runs/session-B.json ...] [--name CD_LG] [--rate 44100]
                  [--access-boost 6] [--pick spinup=2] [--offset 4.21]
    python cut.py RECORDING runs/session-A.json ...      (a recording that isn't beside its log)

Each session's recording is its log's .wav (session.py --mic writes it there),
or LOG=RECORDING. A sound a later session recorded replaces an earlier one's,
so a short session (`session.py --steps room,head,head_sequential`) can redo
some sounds of a long one.

For each recording it finds the two sync beeps at the start (and at the end,
to correct for the recorder's clock), then writes packs/<name>/:

    spinup.wav idle.wav access.wav spindown.wav   what today's HDD Synth plays
    sequential.wav                                its sound for sequential reads
    tray_open.wav tray_close.wav                  what makes it an optical pack (a CD-ROM drive)
    extras/   the rest: seeks by length, the drive's own slow-down, its slow idle, the room, ...
    takes/    every take of every sound, to choose from (then --pick label=N)
    pack.json its name, for the HDD Synth; cut.json where each file came from, and the gain

The synth plays access.wav and sequential.wav as loops over idle.wav, turned
up while the drive is busy, so they must hold only what the activity adds:
they're built from the head on its own (`head` and `head_sequential`: plain
SEEKs with the disc turning slowly and quietly), filtered below 100 Hz, and
set `--access-boost` dB over the idle. Sessions without those steps fall back
to the drive's reads at full speed, which are mostly the spinning.

Levelled against a disk's pack: one gain puts idle.wav at `--idle-rms`
(default 4000, a disk pack's idle), and every file takes it, so the sounds
keep their loudness against each other, except where a file's peak would pass
-1 dBFS: that one stops there (`held` in cut.json). With the loudest peak at
-1 dBFS instead (0.36.0-cd.1), the tray held the pack about 12 dB under a
disk's, and the disk's idle buried it (the Evesham, 22 Nov 2026).
"""
from __future__ import annotations

import argparse
import json
import math
import random
import re
import statistics
import sys
from array import array
from pathlib import Path

import audio

HERE = Path(__file__).resolve().parent

# The pack's own files: (file, label recorded, from, to, loop), times relative
# to the event's start ("s") or end ("e"). The first label recorded wins; the
# access and sequential loops come first from the isolated head (build_*).
PACK = [
    ("spinup.wav", "spinup", ("s", -0.1), ("e", 0.0), False),
    ("idle.wav", "spinning", ("s", 1.0), ("e", -0.5), True),  # full speed, no reads: the drive in use
    ("access.wav", "access_loop_max", ("s", 0.5), ("e", -0.5), True),  # fallbacks: reads at full speed
    ("access.wav", "access", ("s", -0.05), ("e", 0.0), False),
    ("spindown.wav", "spindown", ("s", -0.05), ("e", 0.0), False),
    ("sequential.wav", "read_max", ("s", 1.5), ("s", 5.5), True),
    # An optical pack: a hard disk pack plus the tray, so the synth can play a
    # CD-ROM drive with the same code (only these two files tell them apart).
    ("tray_open.wav", "tray_open", ("s", -0.1), ("e", 0.5), False),
    ("tray_close.wav", "tray_close", ("s", -0.1), ("e", 0.0), False),
]
TAIL_TRIMMED = {"spindown", "auto_spindown"}
# The tray's own sound, as the synth plays it: after spindown.wav (an eject
# brakes the disc first, which the recording holds too), and before
# spinup.wav (closing it clamps the disc, then it spins up and reads).
TRAY_PART = {"tray_open": "last", "tray_close": "first"}
HEAD_HIGHPASS = 100  # Hz: the slow disc's hum (36-48 Hz and its first harmonic) out from under the head
# How often each length of seek comes up in the built access loop: a drive at
# work mostly seeks a little way; full strokes (which can knock the sled
# against its stop) now and then.
SEEK_MIX = {"head_short": 0.5, "head_medium": 0.35, "head_full": 0.15}


class Session:
    """One session: its log, its recording, and where the log's time 0 is."""

    def __init__(self, log_path: Path, recording: Path, rate: int, offset: float | None):
        self.log_path = log_path
        self.log = json.loads(log_path.read_text())
        print(f"reading {recording} ...")
        self.samples = audio.load(recording, rate)
        self.rate = rate
        print(f"  {len(self.samples) / rate:.0f} s at {rate} Hz")
        hz, gap = self.log["beep_hz"], self.log["beep_gap_s"]
        start = offset if offset is not None else audio.find_beeps(self.samples, rate, hz, gap)
        if start is None:
            sys.exit(f"{recording}: no sync beeps in the first 90 s: give --offset (where the first beep starts)")
        self.start, self.scale = start, 1.0
        end_ev = next((e for e in self.log["events"] if e["label"] == "sync_end"), None)
        if end_ev:
            guess = start + end_ev["start"]
            end = audio.find_beeps(self.samples, rate, hz, gap, search_from_s=max(0.0, guess - 5), search_s=10)
            if end is not None:
                self.scale = (end - start) / end_ev["start"]
        drift_ms = (self.scale - 1) * (end_ev["start"] if end_ev else 0) * 1000
        print(f"  sync: first beep at {start:.3f} s; recorder clock {(self.scale - 1) * 1e6:+.0f} ppm "
              f"({drift_ms:+.0f} ms by the end)"
              + ("" if self.scale != 1.0 or not end_ev else " (end beeps not found: assumed exact)"))
        for ev in self.log["events"]:
            ev["_session"] = self

    def window(self, ev, frm, to) -> array:
        t0 = ev["start"] if frm[0] == "s" else ev["end"]
        t1 = ev["start"] if to[0] == "s" else ev["end"]
        return audio.cut(self.samples, self.rate, self.start + (t0 + frm[1]) * self.scale,
                         self.start + (t1 + to[1]) * self.scale)


def bursts(samples, rate: int, step_s: float = 0.05, above_db: float = 8.0, gap_s: float = 0.5):
    """The loud stretches of a take, as (start, end) in seconds: 50 ms windows
    more than `above_db` over its quietest tenth, joined across gaps shorter
    than `gap_s`."""
    env = audio.envelope(samples, rate, step_s)
    if not env:
        return []
    quiet = sorted(env)[len(env) // 10]
    loud = [e > quiet * 10 ** (above_db / 20) for e in env]
    out = []
    for i, on in enumerate(loud):
        if not on:
            continue
        t = i * step_s
        if out and t - out[-1][1] <= gap_s:
            out[-1][1] = t + step_s
        else:
            out.append([t, t + step_s])
    return [tuple(b) for b in out]


def tray_part(samples, rate: int, which: str) -> array:
    """A tray take cut to the tray: its `first` burst (closing: the tray in
    and the disc clamped, before the spin-up) or its `last` (opening: the tray
    out, after the brake), with a little before and its decay after."""
    found = bursts(samples, rate)
    if not found:
        return samples
    if which == "first":
        start, end = found[0]
    else:
        start, end = found[-1]
        if len(found) > 1 and end - start < 0.3:  # a click after the tray stopped: keep the tray with it
            start = found[-2][0]
    return audio.cut(samples, rate, max(0.0, start - 0.15), end + 0.4)


def window(ev, frm, to) -> array:
    return ev["_session"].window(ev, frm, to)


def isolated_seeks(by_label: dict, rate: int) -> list[tuple[str, array]]:
    """Each single head seek (head_short/medium/full), cut to the move itself
    (50 ms before its loudest moment to 300 ms after), filtered below 100 Hz.
    Dropped: a seek with a noisy background just before or after it (more
    than 4 dB over the typical: a sound in the room), or one more than 6 dB
    louder than the typical (a knock)."""
    cands = []
    for label in SEEK_MIX:
        for ev in by_label.get(label, []):
            s = audio.highpass(window(ev, ("s", -0.3), ("e", 0.3)), rate, HEAD_HIGHPASS)
            w = rate // 200
            lv = [audio.rms(s, i, i + w) for i in range(0, len(s) - w, w)]
            if not lv:
                continue
            on = max(range(len(lv)), key=lambda i: lv[i]) * w
            bg = max(audio.rms(s, max(0, on - int(0.30 * rate)), max(1, on - int(0.05 * rate))),
                     audio.rms(s, on + int(0.45 * rate), on + int(0.9 * rate)))
            move = s[max(0, on - int(0.05 * rate)):on + int(0.30 * rate)]
            cands.append((label, ev, audio.fade(move, rate, 0.005, 0.05), bg, audio.peak(move)))
    if not cands:
        return []
    bg_typ = statistics.median(c[3] for c in cands)
    pk_typ = statistics.median(c[4] for c in cands)
    keep = [c for c in cands if c[3] < bg_typ * 10 ** (4 / 20) and c[4] < pk_typ * 10 ** (6 / 20)]
    dropped = [f"{c[0]} #{c[1]['take']}" for c in cands if c not in keep]
    print(f"  head: {len(keep)} of {len(cands)} single seeks used" + (f"; dropped {', '.join(dropped)}" if dropped else ""))
    return [(c[0], c[2]) for c in keep]


def build_access(seeks: list[tuple[str, array]], rate: int, seconds: float = 10.4) -> array:
    """A loop of seeks at random gaps of 150-400 ms, mostly short and medium."""
    rng = random.Random(3)
    by_kind: dict[str, list[array]] = {}
    for label, s in seeks:
        by_kind.setdefault(label, []).append(s)
    kinds = [k for k in SEEK_MIX if k in by_kind]
    weights = [SEEK_MIX[k] for k in kinds]
    out = array("h", bytes(2 * int((seconds + 0.6) * rate)))
    t = 0
    while t < seconds * rate:
        audio.mix_into(out, rng.choice(by_kind[rng.choices(kinds, weights)[0]]), t)
        t += int(rng.uniform(0.15, 0.40) * rate)
    return audio.make_loop(out[:int(seconds * rate)], rate, 0.2)


IDLE_RMS = 4000.0   # a disk pack's idle.wav (HDDSynth_Samples: 3,000-5,000 of 32767)
PEAK_MAX = 32767 * 10 ** (-1 / 20)  # -1 dBFS


def pack_gain(files: dict[str, array], idle_rms: float) -> float:
    """The pack's gain: idle.wav at `idle_rms` (else the loudest peak at -1 dBFS)."""
    idle = files.get("idle.wav")
    if idle is not None and audio.rms(idle) > 0:
        return idle_rms / audio.rms(idle)
    loudest = max((audio.peak(s) for n, s in files.items() if not n.startswith("takes/")), default=1)
    return PEAK_MAX / max(loudest, 1)


def file_gain(s: array, gain: float) -> float:
    """`gain`, or less if it would take the file's peak past -1 dBFS."""
    return min(gain, PEAK_MAX / max(audio.peak(s), 1))


def match_level(s: array, ref: array, boost_db: float) -> array:
    """`s` at `boost_db` over `ref` (RMS)."""
    return audio.scale(s, audio.rms(ref) / max(1.0, audio.rms(s)) * 10 ** (boost_db / 20))


def parse_sessions(args: list[str]) -> list[tuple[Path, Path]]:
    """LOG, LOG=RECORDING, or the old RECORDING LOG pair."""
    pairs = []
    i = 0
    while i < len(args):
        arg = args[i]
        if "=" in arg and arg.split("=", 1)[0].endswith(".json"):
            log, rec = arg.split("=", 1)
            pairs.append((Path(log), Path(rec)))
        elif not arg.endswith(".json") and i + 1 < len(args) and args[i + 1].endswith(".json"):
            pairs.append((Path(args[i + 1]), Path(arg)))
            i += 1
        else:
            pairs.append((Path(arg), Path(arg).with_suffix(".wav")))
        i += 1
    return pairs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sessions", nargs="+", help="session logs (and recordings), earlier first")
    ap.add_argument("--name", help="the pack's folder name (default: CD_ and the drive's model; no spaces)")
    ap.add_argument("--rate", type=int, default=16000, choices=audio.RATES,
                    help="the pack's sample rate: the same as the disk's pack it plays beside (every HDD pack so far: 16000)")
    ap.add_argument("--access-boost", type=float, default=0.0, metavar="DB",
                    help="access.wav and sequential.wav this far over idle.wav (default 0: level with it)")
    ap.add_argument("--idle-rms", type=float, default=IDLE_RMS, metavar="RMS",
                    help=f"idle.wav's level, as a disk pack's (default {IDLE_RMS:.0f}, of 32767)")
    ap.add_argument("--pick", action="append", default=[], help="label=N: use take N of that sound")
    ap.add_argument("--offset", type=float, help="where the first beep starts in the (first) recording, if not found")
    ap.add_argument("--out", default=str(HERE / "packs"))
    a = ap.parse_args()
    rate = a.rate

    sessions = [Session(log, rec, rate, a.offset if k == 0 else None)
                for k, (log, rec) in enumerate(parse_sessions(a.sessions))]
    log = sessions[0].log

    # ---- every take; a later session's takes of a sound replace an earlier one's
    by_label: dict[str, list[dict]] = {}
    for sess in sessions:
        mine: dict[str, list[dict]] = {}
        for ev in sess.log["events"]:
            if not ev["label"].startswith("sync"):
                mine.setdefault(ev["label"], []).append(ev)
        by_label.update(mine)
    picks = dict(p.split("=", 1) for p in a.pick)

    # ---- the room's noise, for trimming spin-downs
    room = next(iter(by_label.get("room", [])), None)
    floor = audio.rms(window(room, ("s", 1), ("e", -1))) if room else 30.0
    print(f"  room noise {audio.db(floor):.0f} dBFS")

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

    # ---- the pack's files: idle first, as the access and sequential loops are set against it
    for name, label, frm, to, loop in PACK:
        if name in files or name in ("access.wav", "sequential.wav"):
            continue
        ev = choose(label)
        if ev is None:
            print(f"  no {label} recorded: {name} left out")
            continue
        s = window(ev, frm, to)
        if label in TAIL_TRIMMED:
            s = audio.trim_tail(s, rate, floor)
        if label in TRAY_PART:
            s = tray_part(s, rate, TRAY_PART[label])
        files[name] = audio.make_loop(s, rate) if loop else audio.fade(s, rate, 0.005, 0.03)
        sources[name] = f"{label} take {ev['take']}" + (", the tray's part" if label in TRAY_PART else "")
    idle = files.get("idle.wav")

    seeks = isolated_seeks(by_label, rate)
    if seeks:
        files["access.wav"] = build_access(seeks, rate)
        sources["access.wav"] = f"built from {len(seeks)} isolated head seeks"
    seq = choose("head_sequential")
    if seq is not None:
        s = audio.highpass(window(seq, ("s", 0.5), ("e", -0.5)), rate, HEAD_HIGHPASS)
        files["sequential.wav"] = audio.make_loop(s, rate)
        sources["sequential.wav"] = f"head_sequential take {seq['take']} (the head alone)"
    for name, label, frm, to, loop in PACK:  # the fallbacks: reads at full speed, mostly the spinning
        if name not in ("access.wav", "sequential.wav") or name in files:
            continue
        ev = choose(label)
        if ev is None:
            continue
        s = window(ev, frm, to)
        files[name] = audio.make_loop(s, rate) if loop else audio.fade(s, rate, 0.005, 0.03)
        sources[name] = f"{label} take {ev['take']} (not isolated: no head steps recorded)"
    if idle is not None:
        for name in ("access.wav", "sequential.wav"):
            if name in files and sources[name].startswith(("built from", "head_sequential")):  # the head alone
                files[name] = match_level(files[name], idle, a.access_boost)
                sources[name] += f", {a.access_boost:+.0f} dB over idle"

    # ---- extras: everything else, and every take
    for label, takes in by_label.items():
        if label == "left_alone":
            # Full speed until the drive's own timer runs out, then it brakes to
            # a slow idle: split where the level falls 6 dB below its first 10 s.
            ev = takes[0]
            s = window(ev, ("s", 0.0), ("e", 0.0))
            step = int(rate * 0.25)
            env = [audio.rms(s, i, i + step) for i in range(0, len(s) - step, step)]
            base = statistics.median(env[:40]) if len(env) > 40 else 0
            drop = next((i for i in range(40, len(env)) if env[i] < base / 2), None)
            if drop is None:
                print("  left_alone: the drive never slowed down by itself (no spin_to_idle, idle_slow)")
            else:
                t = drop * 0.25
                print(f"  left_alone: the drive slowed down by itself {t:.1f} s after the last read")
                files["extras/spin_to_idle.wav"] = audio.fade(window(ev, ("s", t - 1.5), ("s", t + 3.0)), rate)
                if ev["end"] - ev["start"] > t + 6:
                    files["extras/idle_slow.wav"] = audio.make_loop(window(ev, ("s", t + 4.0), ("e", -0.5)), rate)
                    sources["extras/idle_slow.wav"] = "left_alone, after it slowed"
                files["extras/spinning_long.wav"] = audio.make_loop(window(ev, ("s", 1.0), ("s", t - 1.5)), rate)
                sources["extras/spin_to_idle.wav"] = f"left_alone at {t:.1f} s"
                sources["extras/spinning_long.wav"] = "left_alone, before it slowed"
            continue
        loops = ("spinning", "head_loop", "head_sequential")
        if label == "room" or label in loops or label.startswith(("read_", "idle_", "access_loop_")):
            ev = choose(label) if label == "spinning" else takes[0]
            if label == "room":
                s = window(ev, ("s", 0.5), ("e", -0.5))
            else:
                s = audio.make_loop(window(ev, ("s", 1.0), ("e", -0.5)), rate)
            files[f"extras/{label}.wav"] = s
            sources[f"extras/{label}.wav"] = f"{label} take {ev['take']}"
        else:
            ev = choose(label)
            post = 0.5 if label == "tray_open" else 0.0
            s = window(ev, ("s", -0.1), ("e", post))
            if label in TAIL_TRIMMED:
                s = audio.trim_tail(s, rate, floor)
            if label.startswith("head_"):
                s = audio.highpass(s, rate, HEAD_HIGHPASS)
            files[f"extras/{label}.wav"] = audio.fade(s, rate, 0.005, 0.03)
            sources[f"extras/{label}.wav"] = f"{label} take {ev['take']}"
        for ev in takes:
            files[f"takes/{label}_{ev['take']}.wav"] = audio.fade(window(ev, ("s", -0.3), ("e", 0.3)), rate)

    # ---- one gain for the pack: idle.wav at a disk pack's level, each file's
    # peak held to -1 dBFS
    gain = pack_gain(files, a.idle_rms)
    gains = {rel: file_gain(smp, gain) for rel, smp in files.items()}
    held = {rel: round(20 * math.log10(g / gain), 1) for rel, g in gains.items()
            if g < gain and not rel.startswith("takes/")}
    name = a.name or "CD_" + re.sub(r"[^A-Za-z0-9]+", "_", log.get("drive", "drive")).strip("_")[:60]
    out = Path(a.out) / name
    for rel, s in files.items():
        audio.save(out / rel, audio.scale(s, gains[rel]), rate)
    # pack.json holds the HDD Synth's keys only (it reads 1 KB of it; GAIN and
    # AUTHOR can be added by hand); cut.json, how the pack was made.
    synth = {"NAME": " ".join((log.get("drive") or name).split()[:-1] or [name])[:47]}
    (out / "pack.json").write_text(json.dumps(synth, indent=1))
    meta = {"drive": log.get("drive"), "sectors": log.get("sectors"), "max_x": log.get("max_x"),
            "rate": rate, "gain_db": round(20 * math.log10(gain), 1), "access_boost_db": a.access_boost,
            "idle_rms": a.idle_rms, "held_db": held,
            "room_noise_dbfs": round(audio.db(floor * gain), 1),
            "sessions": [{"log": s.log_path.name, "sync_s": round(s.start, 3), "clock_ppm": round((s.scale - 1) * 1e6)}
                         for s in sessions],
            "sync_s": round(sessions[0].start, 3), "clock_ppm": round((sessions[0].scale - 1) * 1e6),
            "sources": sources}
    (out / "cut.json").write_text(json.dumps(meta, indent=1))
    print(f"\n{out}: {len([n for n in files if not n.startswith('takes/')])} files, gain {meta['gain_db']:+.1f} dB")
    for n in sorted(sources):
        secs = len(files[n]) / rate
        print(f"  {n:28} {secs:5.1f} s  {sources[n]}")
    print("\nListen: python preview.py " + str(out))


if __name__ == "__main__":
    main()
