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


def _goertzel(frame, rate: int, hz: float) -> float:
    k = 2 * math.cos(2 * math.pi * hz / rate)
    s1 = s2 = 0.0
    for x in frame:
        s0 = x + k * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - k * s1 * s2


def tone_snr(frame, rate: int, hz: float) -> float:
    """The power at `hz` against the power nearby (at 0.7, 0.8, 1.2 and 1.3
    times it): a pure tone stands far above its neighbours even under a loud
    broadband sound, such as a drive at full speed."""
    near = sum(_goertzel(frame, rate, hz * f) for f in (0.7, 0.8, 1.2, 1.3)) / 4
    return _goertzel(frame, rate, hz) / (near + 1.0)


def find_beeps(samples, rate: int, hz: float, gap_s: float, search_from_s: float = 0.0,
               search_s: float = 90.0, snr: float = 15.0) -> float | None:
    """The start of the first of two beeps `gap_s` apart (start to start), in
    seconds, searching `search_s` from `search_from_s`; None if not found.
    Each 20 ms window is a beep's if `hz` stands `snr` times above the
    frequencies near it; then the start is found to 5 ms."""
    n = int(rate * 0.02)
    first = int(search_from_s * rate)
    last = min(len(samples), first + int(search_s * rate))
    tone = [tone_snr(samples[i:i + n], rate, hz) > snr for i in range(first, last - n, n)]
    gap = int(round(gap_s / 0.02))

    def starts(j: int) -> bool:  # a beep starts here: tone for 40 ms at least, none just before
        return 0 <= j < len(tone) - 1 and tone[j] and tone[j + 1] and (j == 0 or not tone[j - 1])

    for j in range(len(tone) - gap - 3):
        # two beep starts a gap apart, with quiet (no tone) between them: a whistle isn't a sync
        # (quiet at two of three points: a beep's echo can linger into the first)
        # (the second within 80 ms of where it's due: a recording can drop audio as it starts)
        if starts(j) and any(starts(j + gap + d) for d in range(-4, 5)) and sum(
                tone[j + int(gap * f)] for f in (0.6, 0.7, 0.8)) <= 1:
            # to 5 ms: the first window, sliding back from the coarse one, that's still the beep's
            at = first + j * n
            hop = max(1, rate // 200)
            while at - hop >= first and tone_snr(samples[at - hop:at - hop + n], rate, hz) > snr:
                at -= hop
            # that window only has to overlap the beep: its first 5 ms window, at 1 ms steps
            m, ms = max(1, rate // 200), max(1, rate // 1000)
            for i in range(at, at + n + hop, ms):
                if tone_snr(samples[i:i + m], rate, hz) > snr:
                    return i / rate
            return at / rate
    return None


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


def highpass(samples, rate: int, hz: float) -> array:
    """A second-order Butterworth high-pass (RBJ's biquad), run forwards: takes
    the spindle's hum (its tone and first harmonics) out from under a click."""
    w = 2 * math.pi * hz / rate
    alpha = math.sin(w) / math.sqrt(2)
    cw = math.cos(w)
    a0 = 1 + alpha
    b0, b1, b2 = (1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0
    a1, a2 = -2 * cw / a0, (1 - alpha) / a0
    out = array("h", bytes(2 * len(samples)))
    x1 = x2 = y1 = y2 = 0.0
    for i, x in enumerate(samples):
        y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, x, y1, y
        out[i] = 32767 if y > 32767 else -32768 if y < -32768 else int(y)
    return out


def scale(samples, gain: float) -> array:
    return array("h", (max(-32768, min(32767, int(round(s * gain)))) for s in samples))


def mix_into(dest: array, src, at: int, gain: float = 1.0):
    """Adds `src` into `dest` from sample `at` (extending it), clipping."""
    if at + len(src) > len(dest):
        dest.extend(array("h", bytes(2 * (at + len(src) - len(dest)))))
    for i, s in enumerate(src):
        v = dest[at + i] + int(s * gain)
        dest[at + i] = 32767 if v > 32767 else -32768 if v < -32768 else v
