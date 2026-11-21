"""Hear a CD-ROM sample pack in use: scenarios rendered to one WAV and played.

    python preview.py packs/CD_NAME                   # every scenario in turn
    python preview.py packs/CD_NAME game              # one: synth, game, install or audio
    python preview.py packs/CD_NAME game --save g.wav # keep it (and don't play)

    synth    as today's HDD Synth plays any pack: spin up, idle, accesses over
             the idle sound, sequential reads, spin down
    game     a disc going in, a level loading, the drive spinning down when
             left alone, and spinning up again for the next load
    install  a long sequential read with seeks now and then
    audio    CD audio: the disc at 1x, skipping between tracks
"""
from __future__ import annotations

import argparse
import json
import random
import sys
import tempfile
from array import array
from pathlib import Path

import audio


class Pack:
    def __init__(self, folder: Path):
        self.folder = folder
        meta = json.loads((folder / "pack.json").read_text()) if (folder / "pack.json").exists() else {}
        self.rate = meta.get("rate", 44100)
        self.cache: dict[str, array | None] = {}

    def get(self, name: str) -> array | None:
        """`name` without .wav, from the pack or its extras/; None if missing."""
        if name not in self.cache:
            for p in (self.folder / f"{name}.wav", self.folder / "extras" / f"{name}.wav"):
                if p.exists():
                    self.cache[name] = audio.load(p, self.rate)
                    break
            else:
                self.cache[name] = None
        return self.cache[name]

    def first(self, *names: str) -> array | None:
        for n in names:
            s = self.get(n)
            if s is not None:
                return s
        return None


class Timeline:
    """Sounds one after another, joined with a short crossfade, and others
    laid over them (an access over the idle sound)."""

    def __init__(self, rate: int):
        self.rate = rate
        self.out = array("h")
        self.xfade = int(rate * 0.03)

    @property
    def now(self) -> int:
        return len(self.out)

    def play(self, s: array | None):
        if s is None or not len(s):
            return
        if not len(self.out):
            self.out.extend(s)
            return
        x = min(self.xfade, len(s), len(self.out))
        base = len(self.out) - x
        for i in range(x):
            w = i / x
            self.out[base + i] = int(self.out[base + i] * (1 - w) + s[i] * w)
        self.out.extend(s[x:])

    def loop(self, s: array | None, seconds: float):
        if s is None or not len(s):
            return
        want = int(seconds * self.rate)
        filled = array("h")
        while len(filled) < want:
            filled.extend(s)  # the loops are cut to join seamlessly (cut.py)
        self.play(filled[:want])

    def silence(self, seconds: float):
        self.play(array("h", bytes(2 * int(seconds * self.rate))))

    def over(self, s: array | None, at: int, gain: float = 1.0):
        if s is not None:
            audio.mix_into(self.out, s, at, gain)


def synth(p: Pack, t: Timeline, rng: random.Random):
    t.play(p.get("spinup"))
    idle = p.get("idle")
    start = t.now
    t.loop(idle, 14)
    for k in range(5):  # accesses now and then, laid over the idle sound
        t.over(p.get("access"), start + int((1.5 + k * 2.4 + rng.uniform(-0.4, 0.4)) * t.rate))
    t.loop(p.first("sequential", "read_max"), 4)
    t.loop(idle, 3)
    t.play(p.get("spindown"))
    t.silence(1)


def game(p: Pack, t: Timeline, rng: random.Random):
    t.silence(0.5)
    t.play(p.first("tray_close", "spinup"))  # the disc goes in and loads
    t.loop(p.get("idle"), 2)
    for _ in range(3):  # the level loads: a seek, a burst of reading
        t.play(p.first("seek_full", "seek_medium", "access"))
        t.loop(p.first("sequential", "read_max"), rng.uniform(1.0, 2.5))
    t.loop(p.get("idle"), 6)  # playing; the drive left alone
    t.play(p.first("auto_spindown", "spindown"))
    t.silence(3)
    t.play(p.get("spinup"))  # the next level: up to speed again, then load
    for _ in range(2):
        t.play(p.first("seek_medium", "access"))
        t.loop(p.first("sequential", "read_max"), rng.uniform(1.0, 2.0))
    t.loop(p.get("idle"), 3)
    t.play(p.get("spindown"))
    t.silence(1)


def install(p: Pack, t: Timeline, rng: random.Random):
    t.play(p.get("spinup"))
    for _ in range(4):
        t.play(p.first("seek_short", "access"))
        t.loop(p.first("sequential", "read_max"), rng.uniform(3, 6))
    t.loop(p.get("idle"), 3)
    t.play(p.get("spindown"))
    t.silence(1)


def audio_cd(p: Pack, t: Timeline, rng: random.Random):
    t.play(p.get("spinup"))
    one_x = p.first("read_1x", "idle_1x", "read_2x")
    if one_x is None:
        print("  (no 1x recording: SET CD SPEED didn't reach the drive?) using the fastest")
        one_x = p.first("sequential", "idle")
    for _ in range(3):  # a track, then a skip to the next
        t.loop(one_x, rng.uniform(4, 6))
        t.play(p.first("seek_short", "access"))
    t.play(p.get("spindown"))
    t.silence(1)


SCENARIOS = {"synth": synth, "game": game, "install": install, "audio": audio_cd}


def render(pack: Pack, name: str) -> array:
    t = Timeline(pack.rate)
    SCENARIOS[name](pack, t, random.Random(7))
    return t.out


def play(path: Path):
    if sys.platform == "win32":
        import winsound
        winsound.PlaySound(str(path), winsound.SND_FILENAME)
    else:
        import subprocess
        subprocess.run(["afplay" if sys.platform == "darwin" else "aplay", str(path)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pack")
    ap.add_argument("scenario", nargs="?", choices=list(SCENARIOS))
    ap.add_argument("--save", help="write the WAV here instead of playing it")
    a = ap.parse_args()
    pack = Pack(Path(a.pack))
    names = [a.scenario] if a.scenario else list(SCENARIOS)
    for name in names:
        print(f"{name}: rendering ...")
        out = render(pack, name)
        print(f"  {len(out) / pack.rate:.1f} s")
        if a.save:
            path = Path(a.save) if len(names) == 1 else Path(a.save).with_stem(f"{Path(a.save).stem}_{name}")
            audio.save(path, out, pack.rate)
            print(f"  saved {path}")
        else:
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / f"{name}.wav"
                audio.save(path, out, pack.rate)
                play(path)


if __name__ == "__main__":
    main()
