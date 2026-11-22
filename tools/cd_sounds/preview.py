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
import wave
from array import array
from pathlib import Path

import audio


class Pack:
    def __init__(self, folder: Path):
        self.folder = folder
        self.rate = 44100  # the pack's own, from its idle.wav (any pack: an HDD pack too)
        try:
            with wave.open(str(folder / "idle.wav")) as w:
                self.rate = w.getframerate()
        except (OSError, wave.Error, EOFError):
            pass
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

    def running(self, idle: array | None, seconds: float, busy: list[tuple[array | None, float, float]],
                rng: random.Random):
        """The drive running, as the HDD Synth plays it: the idle loop all
        along, and over it each (loop, start, length) in `busy` (access.wav or
        sequential.wav while the drive is busy), from a random point in the
        loop, faded in and out over 50 ms."""
        at = self.now
        self.loop(idle, seconds)
        fade = int(0.05 * self.rate)
        for s, start, length in busy:
            if s is None or not len(s):
                continue
            n = int(length * self.rate)
            k = rng.randrange(len(s))
            piece = array("h")
            while len(piece) < n:
                piece.extend(s[k:])
                k = 0
            piece = piece[:n]
            for i in range(min(fade, n)):
                piece[i] = int(piece[i] * i / fade)
                piece[n - 1 - i] = int(piece[n - 1 - i] * i / fade)
            self.over(piece, at + int(start * self.rate))


def synth(p: Pack, t: Timeline, rng: random.Random):
    """Spin up, the drive running with accesses now and then and a sequential
    read, spin down: today's HDD Synth with this pack."""
    t.play(p.get("spinup"))
    t.running(p.get("idle"), 20, [(p.get("access"), 1.5, 1.2), (p.get("access"), 4.0, 0.6), (p.get("access"), 6.5, 2.5),
                                  (p.get("sequential"), 10.5, 5.0), (p.get("access"), 17.0, 0.8)], rng)
    t.play(p.get("spindown"))
    t.silence(1)


def game(p: Pack, t: Timeline, rng: random.Random):
    """A disc going in, a level loading, the drive left alone until it slows
    down by itself, and spinning up again for the next level."""
    t.silence(0.5)
    t.play(p.first("tray_close", "spinup"))
    t.running(p.get("idle"), 9, [(p.get("access"), 0.5, 1.0), (p.get("sequential"), 1.6, 2.2),
                                 (p.get("access"), 4.0, 0.7), (p.get("sequential"), 4.8, 2.5)], rng)
    t.running(p.get("idle"), 4, [], rng)  # playing; the drive left alone
    t.play(p.first("spin_to_idle", "spindown"))
    t.loop(p.first("idle_slow"), 4)
    t.play(p.get("spinup"))  # the next level: up to speed again, then load
    t.running(p.get("idle"), 6, [(p.get("access"), 0.3, 0.8), (p.get("sequential"), 1.2, 3.0)], rng)
    t.play(p.get("spindown"))
    t.silence(1)


def install(p: Pack, t: Timeline, rng: random.Random):
    """A long sequential read with seeks now and then."""
    t.play(p.get("spinup"))
    busy, at = [], 0.5
    for _ in range(4):
        busy.append((p.get("access"), at, 0.5))
        length = rng.uniform(3, 5)
        busy.append((p.get("sequential"), at + 0.5, length))
        at += length + 0.8
    t.running(p.get("idle"), at + 1, busy, rng)
    t.play(p.get("spindown"))
    t.silence(1)


def audio_cd(p: Pack, t: Timeline, rng: random.Random):
    """CD audio: the disc at 1x where the drive could record it, with a skip
    between tracks."""
    t.play(p.get("spinup"))
    one_x = p.first("read_1x", "idle_1x", "read_2x")
    if one_x is None:
        print("  (no 1x recording: this drive spins at its own speed) using idle.wav")
        one_x = p.get("idle")
    for _ in range(3):
        t.running(one_x, rng.uniform(4, 6), [(p.get("access"), 0.0, 0.6)], rng)
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
