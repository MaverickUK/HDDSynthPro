"""What differs between Windows and macOS: the CD drive's transport
(scsi_win.py, scsi_mac.py), the sync beep, and recording from a microphone
with ffmpeg (DirectShow on Windows, AVFoundation on macOS).
"""
from __future__ import annotations

import math
import subprocess
import sys
import time
import wave
from array import array
from pathlib import Path

MAC = sys.platform == "darwin"
HERE = Path(__file__).resolve().parent


# ---- The drive -------------------------------------------------------------------------


def list_cd_drives() -> list[tuple[str, str, str]]:
    """(path, short name, name) for each CD drive: `\\\\.\\E:`, `E:` on Windows;
    the index and `diskN` on macOS."""
    if MAC:
        from scsi_mac import list_cd_drives as found
    else:
        from scsi_win import list_cd_drives as found
    return found()


def open_drive(path: str):
    if MAC:
        from scsi_mac import CdDrive
    else:
        from scsi_win import CdDrive
    return CdDrive(path)


def pick_drive(arg: str | None) -> str:
    """The drive `--drive` names, else the only one there is (listing them)."""
    drives = list_cd_drives()
    for path, short, name in drives:
        print(f"  {path if MAC else short}  {name}" + (f"  ({short})" if MAC else ""))
    if arg:
        return arg
    if len(drives) == 1:
        return drives[0][0]
    sys.exit("give --drive (none found)" if not drives else "more than one CD drive: give --drive")


DRIVE_HELP = ("the drive: its number or diskN (macOS), \\\\.\\CdRomN or a drive letter (Windows); "
              "default the only one")


# ---- The sync beep -----------------------------------------------------------------------


def beep(hz: int, ms: int):
    """A tone, returning when it's done (as Windows' Beep does)."""
    if not MAC:
        import winsound
        winsound.Beep(hz, ms)
        return
    path = HERE / "runs" / f"beep-{hz}-{ms}.wav"
    if not path.exists():
        path.parent.mkdir(exist_ok=True)
        rate = 48000
        n = rate * ms // 1000
        ramp = rate // 200  # 5 ms in and out, so it doesn't click
        xs = array("h", (int(20000 * min(1.0, i / ramp, (n - i) / ramp) * math.sin(2 * math.pi * hz * i / rate))
                         for i in range(n)))
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(xs.tobytes())
    subprocess.run(["afplay", str(path)])


# ---- Recording from a microphone (ffmpeg) ------------------------------------------


def mic_names() -> list[str]:
    """The microphones ffmpeg can record from."""
    if MAC:
        p = subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
                           capture_output=True, text=True)
        return parse_avfoundation_list(p.stderr)
    p = subprocess.run(["ffmpeg", "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
                       capture_output=True, text=True)
    # lines like: [dshow @ 0000...] "Microphone (Samson Meteor Mic)" (audio)
    return [line.split("]", 1)[-1].strip().removesuffix("(audio)").strip().strip('"')
            for line in p.stderr.splitlines() if "(audio)" in line]


def parse_avfoundation_list(text: str) -> list[str]:
    """The audio devices from ffmpeg's AVFoundation list (`[n] Name` lines
    after `AVFoundation audio devices:`)."""
    names, audio = [], False
    for line in text.splitlines():
        if "AVFoundation audio devices" in line:
            audio = True
        elif "AVFoundation video devices" in line:
            audio = False
        elif audio and "] [" in line:
            names.append(line.split("] [", 1)[1].split("] ", 1)[1].strip())
    return names


def mic_input(mic: str) -> list[str]:
    """ffmpeg's input arguments for recording from `mic`."""
    if MAC:
        return ["-f", "avfoundation", "-i", f":{mic}"]
    return ["-f", "dshow", "-audio_buffer_size", "50", "-i", f"audio={mic}"]


def start_recording(mic: str, path: Path, rate: int) -> subprocess.Popen:
    cmd = (["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"] + mic_input(mic)
           + ["-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", str(path)])
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    time.sleep(2)  # ffmpeg opens the device
    if p.poll() is not None:
        sys.exit(f"ffmpeg couldn't record from {mic!r} (python session.py --list-mics)")
    return p


def stop_recording(p: subprocess.Popen):
    try:
        p.communicate(b"q", timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
