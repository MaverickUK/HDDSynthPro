"""Audio for the sample pack, with the standard library only (and ffmpeg to
read whatever the recorder made): mono 16-bit samples in `array('h')`.

The HDD Synth's packs are mono, 16-bit, one sample rate per pack (16, 22.05,
32, 44.1 or 48 kHz): HDDSynth_Samples' README.
"""
from __future__ import annotations

import math
import shutil
import subprocess
import tempfile
import wave
from array import array
from pathlib import Path

RATES = (16000, 22050, 32000, 44100, 48000)


def load(path: str | Path, rate: int) -> array:
    """Any audio file ffmpeg reads, as mono 16-bit at `rate`."""
    path = Path(path)
    if _is_plain(path, rate):
        return _read_wav(path)
    if not shutil.which("ffmpeg"):
        raise SystemExit(f"{path} isn't mono 16-bit at {rate} Hz, and ffmpeg (to convert it) isn't found")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "plain.wav"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(path), "-ac", "1",
                        "-ar", str(rate), "-c:a", "pcm_s16le", str(out)], check=True)
        return _read_wav(out)


def _is_plain(path: Path, rate: int) -> bool:
    if path.suffix.lower() != ".wav":
        return False
    try:
        with wave.open(str(path)) as w:
            return w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == rate
    except (wave.Error, EOFError):
        return False


def _read_wav(path: Path) -> array:
    with wave.open(str(path)) as w:
        a = array("h")
        a.frombytes(w.readframes(w.getnframes()))
        return a


def save(path: str | Path, samples: array, rate: int):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.tobytes())


# ---- Levels ------------------------------------------------------------------------


def rms(samples, start: int = 0, end: int | None = None) -> float:
    end = len(samples) if end is None else min(end, len(samples))
    n = end - start
    if n <= 0:
        return 0.0
    return math.sqrt(sum(s * s for s in samples[start:end]) / n)


def peak(samples) -> int:
    return max((abs(s) for s in samples), default=0)


def envelope(samples, rate: int, window_s: float = 0.05) -> list[float]:
    """RMS per window."""
    w = max(1, int(rate * window_s))
    return [rms(samples, i, i + w) for i in range(0, len(samples), w)]


def db(level: float) -> float:
    return 20 * math.log10(max(level, 1e-9) / 32768)


# ---- Finding the sync beeps -----------------------------------------------------------


def tone_power(frame, rate: int, hz: float) -> float:
    """Goertzel: the power at `hz` in `frame`, as a share of the frame's
    energy (1.0: a pure tone)."""
    k = 2 * math.cos(2 * math.pi * hz / rate)
    s1 = s2 = 0.0
    energy = 0.0
    for x in frame:
        s0 = x + k * s1 - s2
        s2, s1 = s1, s0
        energy += x * x
    if energy <= 0:
        return 0.0
    power = s1 * s1 + s2 * s2 - k * s1 * s2
    return power / (energy * len(frame) / 2)


def find_beeps(samples, rate: int, hz: float, gap_s: float, search_from_s: float = 0.0,
               search_s: float = 90.0, frame_s: float = 0.01) -> float | None:
    """The start of the first of two beeps `gap_s` apart (start to start), in
    seconds, searching `search_s` from `search_from_s`; None if not found."""
    n = int(rate * frame_s)
    first = int(search_from_s * rate)
    last = min(len(samples), first + int(search_s * rate))
    tone: list[bool] = []
    for i in range(first, last - n, n):
        frame = samples[i:i + n]
        # loud enough (above -44 dBFS) and mostly the beep's pitch
        tone.append(rms(frame) > 200 and tone_power(frame, rate, hz) > 0.5)
    gap = int(round(gap_s / frame_s))

    def starts(j: int) -> bool:  # a beep starts here: tone now and next, none just before
        return 0 <= j < len(tone) - 1 and tone[j] and tone[j + 1] and (j == 0 or not tone[j - 1])

    for j in range(len(tone) - gap - 3):
        # two beep starts a gap apart, with quiet (no tone) between them: a whistle isn't a sync
        if starts(j) and any(starts(j + gap + d) for d in (-2, -1, 0, 1, 2)) and not tone[j + int(gap * 0.7)]:
            return _onset(samples, rate, first + j * n, n) / rate
    return None


def _onset(samples, rate: int, at: int, frame: int) -> int:
    """The beep's first sample, to about a millisecond: the first 1 ms window
    from a frame before `at` whose level is half the beep's own."""
    ms = max(1, rate // 1000)
    level = rms(samples, at + frame, at + 3 * frame)  # inside the beep
    for i in range(max(0, at - frame), at + frame, ms):
        if rms(samples, i, i + ms) > level / 2:
            return i
    return at


# ---- Editing ---------------------------------------------------------------------------


def cut(samples, rate: int, start_s: float, end_s: float) -> array:
    a, b = max(0, int(start_s * rate)), min(len(samples), int(end_s * rate))
    return array("h", samples[a:b])


def fade(samples, rate: int, in_s: float = 0.005, out_s: float = 0.005) -> array:
    out = array("h", samples)
    n_in, n_out = min(len(out), int(rate * in_s)), min(len(out), int(rate * out_s))
    for i in range(n_in):
        out[i] = int(out[i] * i / n_in)
    for i in range(n_out):
        out[len(out) - 1 - i] = int(out[len(out) - 1 - i] * i / n_out)
    return out


def make_loop(samples, rate: int, xfade_s: float = 0.4) -> array:
    """A loop that joins without a click: the end is crossfaded into the
    start, so playing it over and over is seamless (as the synth loops idle)."""
    x = min(int(rate * xfade_s), len(samples) // 3)
    n = len(samples) - x
    out = array("h", samples[:n])
    for i in range(x):
        w = i / x  # equal-power would suit unrelated sounds; these are the same steady noise
        out[i] = int(samples[i] * w + samples[n + i] * (1 - w))
    return out


def trim_tail(samples, rate: int, floor: float, margin_s: float = 0.3, factor: float = 1.6) -> array:
    """Cut a sound once it has fallen to the room's noise (`floor`, RMS), plus
    a margin: a spin-down's end."""
    w = int(rate * 0.05)
    end = len(samples)
    for i in range(len(samples) - w, 0, -w):
        if rms(samples, i, i + w) > floor * factor:
            end = min(len(samples), i + w + int(rate * margin_s))
            break
    return array("h", samples[:end])


def scale(samples, gain: float) -> array:
    return array("h", (max(-32768, min(32767, int(round(s * gain)))) for s in samples))


def mix_into(dest: array, src, at: int, gain: float = 1.0):
    """Adds `src` into `dest` from sample `at` (extending it), clipping."""
    if at + len(src) > len(dest):
        dest.extend(array("h", bytes(2 * (at + len(src) - len(dest)))))
    for i, s in enumerate(src):
        v = dest[at + i] + int(s * gain)
        dest[at + i] = 32767 if v > 32767 else -32768 if v < -32768 else v
