"""Listening to the drive from this PC: record from the microphone while the
drive does something, then measure what was heard (the level, and the disc's
rotation tone), so a session's steps can be checked before a long take.

    from listen import Listener
    with Listener("Microphone (Samson Meteor Mic)") as ear:
        ear.mark("reading"); ...           # do things, marking when
    print(ear.report())                    # level and spindle tone per half second
"""
from __future__ import annotations

import math
import time
from pathlib import Path

import audio
from session import start_recording, stop_recording

HERE = Path(__file__).resolve().parent
RATE = 44100


def tone_at(samples, hz: float) -> float:
    k = 2 * math.cos(2 * math.pi * hz / RATE)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + k * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / max(1, len(samples))


def spindle(samples, lo: int = 40, hi: int = 170, step: int = 2) -> tuple[int, float]:
    """The strongest tone between `lo` and `hi` Hz, and how far it stands above
    the median there (a spinning disc: a strong peak at its turns a second)."""
    spectrum = [(hz, tone_at(samples, hz)) for hz in range(lo, hi, step)]
    median = sorted(v for _, v in spectrum)[len(spectrum) // 2] or 1e-9
    hz, v = max(spectrum, key=lambda t: t[1])
    return hz, v / median


class Listener:
    def __init__(self, mic: str, name: str = "listen"):
        self.mic = mic
        self.path = HERE / "runs" / f"{name}-{time.strftime('%H%M%S')}.wav"
        self.path.parent.mkdir(exist_ok=True)
        self.marks: list[tuple[float, str]] = []

    def __enter__(self):
        self.proc = start_recording(self.mic, self.path, RATE)
        self.t0 = time.perf_counter()
        return self

    def mark(self, label: str):
        self.marks.append((time.perf_counter() - self.t0, label))

    def __exit__(self, *exc):
        time.sleep(0.3)
        stop_recording(self.proc)

    def report(self, step_s: float = 0.5) -> str:
        s = audio.load(self.path, RATE)
        lead = len(s) / RATE - (time.perf_counter() - self.t0)  # ffmpeg started before t0
        lead = max(0.0, lead)
        lines, marks = [], list(self.marks)
        n = int(step_s * RATE)
        for i in range(int(lead * RATE), len(s) - n, n):
            t = i / RATE - lead
            while marks and marks[0][0] <= t + step_s / 2:
                lines.append(f"        --- {marks.pop(0)[1]}")
            chunk = s[i:i + min(n, RATE // 4)]
            hz, strength = spindle(chunk)
            spin = f"{hz:3d} Hz x{strength:3.0f}" if strength > 6 else ""
            lines.append(f"{t:6.1f} s {audio.db(audio.rms(s, i, i + n)):6.1f} dBFS  {spin}")
        return "\n".join(lines)
