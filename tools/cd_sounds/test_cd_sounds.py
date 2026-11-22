"""Tests for the CD-ROM sound tools, with no drive and no microphone:

    cd tools/cd_sounds && python -m pytest -q
"""
from __future__ import annotations

import json
import math
import random
import shutil
import subprocess
import sys
from array import array
from pathlib import Path

import pytest

import audio
import scsi
from drive_ops import Ops
from session import BEEP_GAP_S, BEEP_HZ, BEEP_MS, FakeDrive

HERE = Path(__file__).resolve().parent
RATE = 44100


# ---- Commands and answers ------------------------------------------------------------


def test_command_blocks():
    assert scsi.start_stop(True) == bytes([0x1B, 0, 0, 0, 1, 0])           # spin up
    assert scsi.start_stop(False) == bytes([0x1B, 0, 0, 0, 0, 0])          # spin down
    assert scsi.start_stop(False, load_eject=True) == bytes([0x1B, 0, 0, 0, 2, 0])  # open the tray
    assert scsi.start_stop(True, load_eject=True) == bytes([0x1B, 0, 0, 0, 3, 0])   # close it
    assert scsi.read10(0x12345, 32) == bytes([0x28, 0, 0, 1, 0x23, 0x45, 0, 0, 32, 0])
    assert scsi.set_cd_speed(706) == bytes([0xBB, 0, 0x02, 0xC2, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0])
    assert len(scsi.mode_sense10(0x2A)) == 10 and scsi.mode_sense10(0x2A)[2] == 0x2A


def test_answers():
    sense = scsi.parse_sense(bytes([0x70, 0, 0x02, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x3A, 0x00] + [0] * 4))
    assert (sense.key, sense.asc) == (2, 0x3A) and "no disc" in str(sense)
    assert scsi.parse_sense(bytes([0x70, 0, 0x05, 0] + [0] * 8 + [0x20, 0])).unsupported
    assert scsi.parse_capacity((332_999).to_bytes(4, "big") + (2048).to_bytes(4, "big")) == (333_000, 2048)
    ops = Ops(FakeDrive(fast=True), log=lambda *a: None)
    info = ops.info()
    assert info.name.startswith("FAKE") and round(info.max_x) == 40 and info.tray == "tray"
    assert info.sectors == 300_000


def test_radius_and_lba_agree():
    for lba in (0, 50_000, 166_500, 332_999):
        assert abs(scsi.lba_at_radius(scsi.radius_mm(lba)) - lba) < 50
    assert scsi.radius_mm(0) == 25.0 and abs(scsi.radius_mm(333_000) - 58.0) < 0.01


# ---- Audio ---------------------------------------------------------------------------------


def tone(seconds: float, hz: float, level: float) -> list[float]:
    return [level * math.sin(2 * math.pi * hz * i / RATE) for i in range(int(seconds * RATE))]


def noise(seconds: float, level: float, rng: random.Random) -> list[float]:
    return [rng.gauss(0, level) for _ in range(int(seconds * RATE))]


def to_samples(xs) -> array:
    return array("h", (max(-32768, min(32767, int(x))) for x in xs))


def test_beeps_are_found_in_noise():
    rng = random.Random(1)
    sig = noise(3.217, 300, rng)
    for k in range(2):
        sig += [b + n for b, n in zip(tone(BEEP_MS / 1000, BEEP_HZ, 8000), noise(BEEP_MS / 1000, 300, rng))]
        sig += noise(BEEP_GAP_S - BEEP_MS / 1000, 300, rng)
    sig += noise(2, 300, rng)
    found = audio.find_beeps(to_samples(sig), RATE, BEEP_HZ, BEEP_GAP_S)
    assert found is not None and abs(found - 3.217) < 0.011


def test_a_single_beep_or_a_whistle_isnt_taken_for_the_sync():
    rng = random.Random(2)
    sig = noise(1, 300, rng) + tone(0.2, BEEP_HZ, 8000) + noise(2, 300, rng) + tone(3, BEEP_HZ, 8000)
    assert audio.find_beeps(to_samples(sig), RATE, BEEP_HZ, BEEP_GAP_S) is None


def test_loop_and_tail():
    rng = random.Random(3)
    s = to_samples(noise(2, 3000, rng))
    loop = audio.make_loop(s, RATE, 0.4)
    assert len(loop) == len(s) - int(0.4 * RATE)
    # the join: the loop's last sample runs on into its first as the original did
    assert loop[-1] == s[len(loop) - 1] and abs(loop[0] - s[len(loop)]) < 50
    decay = to_samples([x * math.exp(-i / RATE * 3) for i, x in enumerate(noise(4, 6000, rng))])
    trimmed = audio.trim_tail(decay, RATE, floor=200)
    assert 0.5 * RATE < len(trimmed) < 3.0 * RATE


# ---- A whole pack from a synthetic recording ------------------------------------------------

SOUNDS = {  # label: (hz, level): each sound a tone of its own, so a cut in the wrong place shows
    "room": (0, 0), "spinup": (300, 6000), "spinning": (400, 3000), "left_alone": (138, 5000), "spindown": (250, 5000),
    "read_max": (600, 4000), "idle_1x": (150, 2000), "read_1x": (180, 2500), "access": (800, 7000),
    "seek_short": (900, 5000), "loading": (650, 4000), "tray_open": (1200, 6000), "tray_close": (1100, 6000),
}


def synthetic_session() -> dict:
    t, events = 0.0, [{"label": "sync_start", "take": 1, "start": 0.0, "end": 0.7, "detail": {}}]
    t = 2.0
    plan = [("room", 1, 4.0)] + [(lbl, k, d) for k in (1, 2, 3) for lbl, d in
                                 (("spinup", 2.5), ("spinning", 6.0), ("spindown", 4.0))]
    plan += [("left_alone", 1, 20.0)]
    plan += [("read_1x", 1, 6.0), ("idle_1x", 1, 5.0), ("read_max", 1, 7.0)]
    plan += [(lbl, k, d) for k in (1, 2, 3) for lbl, d in
             (("access", 1.0), ("seek_short", 0.8), ("loading", 3.0), ("tray_open", 1.5), ("tray_close", 3.0))]
    for label, take, d in plan:
        events.append({"label": label, "take": take, "start": round(t, 3), "end": round(t + d, 3), "detail": {}})
        t += d + 1.5
    events.append({"label": "sync_end", "take": 1, "start": round(t, 3), "end": round(t + 0.7, 3), "detail": {}})
    return {"drive": "FAKE CD-ROM 1.00", "sectors": 300000, "max_x": 40, "beep_hz": BEEP_HZ, "beep_ms": BEEP_MS,
            "beep_gap_s": BEEP_GAP_S, "events": events}


def render_recording(log: dict, offset: float, clock: float, rate: int) -> list[float]:
    """The session as a recorder would have heard it: room noise throughout,
    the beeps, each event its own tone (spin-downs decaying)."""
    rng = random.Random(4)
    end = offset + (log["events"][-1]["end"] + 2) * clock
    out = [rng.gauss(0, 60) for _ in range(int(end * rate))]

    def lay(at_s: float, xs):
        i0 = int(at_s * rate)
        for i, x in enumerate(xs):
            if i0 + i < len(out):
                out[i0 + i] += x

    for ev in log["events"]:
        at = offset + ev["start"] * clock
        d = (ev["end"] - ev["start"]) * clock
        if ev["label"].startswith("sync"):
            for k in range(2):
                lay(at + k * BEEP_GAP_S * clock, [8000 * math.sin(2 * math.pi * BEEP_HZ * i / rate)
                                                   for i in range(int(BEEP_MS / 1000 * rate))])
            continue
        hz, level = SOUNDS[ev["label"]]
        if hz:
            decay = ev["label"] == "spindown"
            brake = ev["label"] == "left_alone"  # full level for 12 s, then a tenth (the drive's slow idle)
            lay(at, [level * math.sin(2 * math.pi * hz * i / rate) * (math.exp(-3 * i / rate) if decay else 1)
                     * (0.1 if brake and i > 12 * rate else 1) for i in range(int(d * rate))])
    return out


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="ffmpeg converts the stereo recording")
def test_a_recording_cuts_into_a_pack_and_every_scenario_renders(tmp_path):
    log = synthetic_session()
    log_path = tmp_path / "session.json"
    log_path.write_text(json.dumps(log))
    rate_in = 48000
    xs = render_recording(log, offset=3.4, clock=1.0002, rate=rate_in)  # the recorder's clock 200 ppm fast
    stereo = array("h")
    for x in to_samples(xs):
        stereo.extend((x, x))
    rec = tmp_path / "recording.wav"
    import wave
    with wave.open(str(rec), "wb") as w:  # stereo 48 kHz, as a recorder might make it
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate_in)
        w.writeframes(stereo.tobytes())

    out = tmp_path / "packs"
    p = subprocess.run([sys.executable, str(HERE / "cut.py"), str(rec), str(log_path), "--out", str(out),
                        "--name", "CD_Test"], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    import re
    ppm = int(re.search(r"recorder clock ([+-]\d+) ppm", p.stdout).group(1))
    assert abs(ppm - 200) <= 20, p.stdout  # beeps found to about a millisecond, 150 s apart
    pack = out / "CD_Test"
    for name in ("spinup", "idle", "access", "spindown", "sequential", "tray_open", "tray_close"):
        assert (pack / f"{name}.wav").exists(), name
    meta = json.loads((pack / "cut.json").read_text())
    assert json.loads((pack / "pack.json").read_text())["NAME"] == "FAKE CD-ROM"
    assert abs(meta["sync_s"] - 3.4) < 0.011 and meta["rate"] == 16000

    # Each file is its own sound: the idle loop is 400 Hz, access 800 Hz
    for name, hz in (("idle", 400), ("access", 800), ("sequential", 600)):
        s = audio.load(pack / f"{name}.wav", RATE)
        mid = s[len(s) // 2 - 2205:len(s) // 2 + 2205]
        assert audio.tone_power(mid, RATE, hz) > 0.8, name
    spindown = audio.load(pack / "spindown.wav", RATE)
    assert len(spindown) < 4.0 * RATE  # trimmed once it fell into the room's noise
    assert len(list((pack / "takes").glob("spinup_*.wav"))) == 3
    assert (pack / "extras" / "tray_close.wav").exists() and (pack / "extras" / "read_1x.wav").exists()
    assert "slowed down by itself 12.0 s" in p.stdout or "slowed down by itself 12.2 s" in p.stdout, p.stdout
    for name in ("spin_to_idle", "idle_slow", "spinning_long"):
        assert (pack / "extras" / f"{name}.wav").exists(), name

    import preview
    pk = preview.Pack(pack)
    for scenario in preview.SCENARIOS:
        rendered = preview.render(pk, scenario)
        assert len(rendered) > 5 * RATE and audio.peak(rendered) > 1000, scenario
