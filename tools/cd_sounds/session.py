"""A timed recording session: the CD-ROM drive makes each of its sounds a few
times, with quiet between, and every one is logged against two sync beeps so
cut.py can cut the recording into a sample pack.

    python session.py --list-mics                     # microphones ffmpeg can record
    python session.py --mic "Microphone (USB Audio)"  # run it and record from that mic
    python session.py                                 # run it; record on your own recorder
    python session.py --tray --idle-timeout 60        # the tray too; leave the drive alone 60 s (45 by default)
    python session.py --dry-run --fast                # a fake drive: check the plan and the log

Start your recorder first (if it's not this PC's), with the PC's speakers
loud enough for it to hear the beeps; keep still and quiet during the run.
Writes runs/session-<time>.json (and .wav with --mic) beside this script.
"""
from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import scsi
from drive_ops import Ops

HERE = Path(__file__).resolve().parent
BEEP_HZ, BEEP_MS, BEEP_GAP_S = 1000, 200, 0.5  # two beeps, starts 0.5 s apart


@dataclass
class Event:
    label: str        # what was recorded: spinup, spindown, idle_max, read_8x, seek_full, access, ...
    take: int
    start: float      # seconds after the first beep started
    end: float
    detail: dict = field(default_factory=dict)


class Session:
    def __init__(self, ops: Ops, quiet_s: float, takes: int, fast: bool = False, beep=None):
        self.ops = ops
        self.quiet_s = quiet_s
        self.takes = takes
        self.fast = fast  # dry runs: every wait cut to a tenth
        self.events: list[Event] = []
        self.t0 = 0.0
        self.beep = beep or _beep
        self.rng = random.Random(1)

    # ---- timing ------------------------------------------------------------------

    def now(self) -> float:
        return time.perf_counter() - self.t0

    def wait(self, seconds: float):
        time.sleep(seconds / 10 if self.fast else seconds)

    def quiet(self, seconds: float | None = None):
        self.wait(self.quiet_s if seconds is None else seconds)

    def mark(self, label: str, take: int, start: float, **detail) -> Event:
        e = Event(label, take, round(start, 3), round(self.now(), 3), detail)
        self.events.append(e)
        print(f"  {e.start:8.2f}-{e.end:8.2f}  {label} #{take} {detail if detail else ''}")
        return e

    def sync(self, label: str):
        """Two beeps: the first's start is time 0 (at the session's start)."""
        if label == "start":
            self.t0 = time.perf_counter()
        start = self.now()
        self.beep(BEEP_HZ, BEEP_MS)
        while self.now() < start + BEEP_GAP_S:
            time.sleep(0.005)
        self.beep(BEEP_HZ, BEEP_MS)
        self.mark(f"sync_{label}", 1, start)

    # ---- the sounds --------------------------------------------------------------

    def stop_and_settle(self):
        self.ops.spin_down()
        self.wait(10)  # a disc at speed takes several seconds to coast to a stop

    def room(self):
        """Nothing turning: the room's own noise, for cut.py's noise floor."""
        self.stop_and_settle()
        start = self.now()
        self.wait(5)
        self.mark("room", 1, start)

    def warm(self, seconds: float = 7.0):
        """Reads long enough for the disc to reach full speed: drives run
        slowly when idle, climb to full speed under steady reads (the LG
        GCC-4320B: about 5 s from rest) and stay there for a while after the
        last read (30 s). 21 Nov 2026, measured by the disc's tone."""
        self.ops.read_for(seconds / 10 if self.fast else seconds, self.ops.sectors // 3)

    def spinups(self):
        """From rest: spin up while reading (8 s: the climb to full speed,
        about 5 s on the LG), full speed left alone (10 s, no reads: inside
        the drive's own timer), then spin down on command and coast to rest.
        The HDD Synth's spinup.wav, idle.wav and spindown.wav."""
        self.ops.set_speed(None)
        for take in range(1, self.takes + 1):
            start = self.now()
            self.ops.spin_up()
            self.ops.wait_ready(30)
            self.ops.read_for(max(0.1, 8 - (self.now() - start)) / (10 if self.fast else 1), self.ops.sectors // 3)
            self.mark("spinup", take, start)
            start = self.now()
            self.wait(10)
            self.mark("spinning", take, start)
            start = self.now()
            self.ops.spin_down()
            self.wait(8)
            self.mark("spindown", take, start)
            self.quiet()

    def left_alone(self, seconds: float):
        """Read, then nothing: the drive at full speed until its own timer
        runs out, then it brakes to its slow idle speed. One long take;
        cut.py finds the moment it slows (`spin_to_idle`, `idle_slow`)."""
        self.warm()
        start = self.now()
        self.wait(seconds)
        self.mark("left_alone", 1, start, waited_s=seconds)

    def speeds(self, max_x: float):
        """Reading at each speed, and spinning idle after it."""
        mid = self.ops.sectors // 3
        speeds = [s for s in (1, 2, 4, 8, 16, 24, 32, 48) if s < max_x - 1]
        if not self.fast and not self.ops.speed_obeyed():
            print("  the drive ignores the speed asked: recording it at its own speed only")
            speeds = []
        for x in speeds + [None]:
            name = f"{x}x" if x else "max"
            r = self.ops.set_speed(x)
            self.ops.wait_ready(30)
            self.warm()  # to the new speed
            start = self.now()
            n = self.ops.read_for(8 if not self.fast else 0.8, mid)
            self.mark(f"read_{name}", 1, start, sectors=n, speed_set=r.ok)
            start = self.now()
            self.wait(6)
            self.mark(f"idle_{name}", 1, start)
            self.quiet(1.5)

    def seeks(self):
        """Single seeks of three lengths, with the drive at full speed (warmed
        before each kind: each lasts well under the drive's 30 s)."""
        self.ops.set_speed(None)
        self.ops.wait_ready(30)
        top = self.ops.max_radius()
        mid = (25 + top) / 2
        kinds = (("seek_short", mid, 0.6, 1.0), ("seek_medium", mid - 6, 12.0, 1.2), ("seek_full", 25.5, top - 26, 1.5))
        for label, base, span, gap in kinds:
            self.warm()
            self.ops.seek_radius(base)
            self.quiet(1.5)
            for take in range(1, 2 * self.takes + 1):
                r = base + (span if take % 2 else 0) + self.rng.uniform(-0.2, 0.2)
                start = self.now()
                self.ops.seek_radius(r)
                self.wait(gap)
                self.mark(label, take, start, radius_mm=round(r, 1))
            self.quiet(2)

    def access(self):
        """Bursts of random seeks, each with a short read: the HDD Synth's
        access.wav, as a game or a directory listing makes."""
        self.warm()
        for take in range(1, self.takes + 1):
            start = self.now()
            for _ in range(6):
                r = self.rng.uniform(25, self.ops.max_radius())
                self.ops.read_at(scsi.lba_at_radius(r), 16)
            self.wait(0.3)
            self.mark("access", take, start)
            self.quiet(2.5)

    def loading(self):
        """A program loading: a seek, then a few seconds of reading, again:
        the HDD Synth's sequential.wav, and material for simulations."""
        self.warm()
        for take in range(1, self.takes + 1):
            start = self.now()
            for _ in range(3):
                lba = scsi.lba_at_radius(self.rng.uniform(25, self.ops.max_radius()))
                self.ops.read_at(lba, 1)
                end = time.perf_counter() + (2 if not self.fast else 0.2)
                while time.perf_counter() < end:
                    self.ops.read_at(lba, 32)
                    lba += 32
            self.mark("loading", take, start)
            self.quiet(2)

    def tray(self):
        """The tray out and in; closing it loads the disc: the drive spins up
        and reads its table of contents, then Windows reads the disc, which
        the LG does at full speed (a few seconds after it's ready)."""
        for take in range(1, self.takes + 1):
            start = self.now()
            self.ops.open_tray()
            self.wait(1.5)
            self.mark("tray_open", take, start)
            self.quiet(2)
            start = self.now()
            self.ops.close_tray()
            ready = self.ops.wait_ready(40)
            self.wait(8)
            self.mark("tray_close", take, start, ready_s=round(ready, 2))
            self.quiet()

    def access_loops(self):
        """Nonstop random seeks with short reads, 12 s a take: the HDD Synth
        plays access.wav as a loop for as long as the drive is busy (turned up
        and down by activity, over the idle loop), so it needs a busy stretch,
        not one burst. At full speed and at the drive's medium speed (the LG
        takes 0xFFFF as 5,400 rpm), where the seeks may stand out more."""
        for name, kbs in (("max", None), ("medium", 0xFFFF)):
            if kbs is None:
                self.ops.set_speed(None)
            else:
                self.ops.drive.command(scsi.set_cd_speed(kbs))
            self.warm()
            for take in range(1, 3):
                start = self.now()
                end = time.perf_counter() + (1.2 if self.fast else 12)
                while time.perf_counter() < end:
                    self.ops.read_at(scsi.lba_at_radius(self.rng.uniform(25, self.ops.max_radius())), 16)
                self.mark(f"access_loop_{name}", take, start, speed=name)
                self.quiet()
        self.ops.set_speed(None)

    def head(self):
        """The head on its own: plain SEEKs (no reads) with the disc stopped
        first. The LG then turns the disc only slowly (about 2,200 rpm, barely
        above the room) and each seek stands 11-14 dB clear of it (21 Nov
        2026), where reads keep it at full speed and bury the seeks. For an
        access loop laid over the idle, as the HDD Synth plays it."""
        self.stop_and_settle()
        top = scsi.lba_at_radius(self.ops.max_radius()) - 100
        self.ops.drive.command(scsi.seek10(top // 2), timeout_s=30)  # the disc starts, slowly
        self.wait(3)
        for take in range(1, 3):
            start = self.now()
            end = time.perf_counter() + (1.2 if self.fast else 12)
            while time.perf_counter() < end:
                self.ops.drive.command(scsi.seek10(int(self.rng.uniform(100, top))))
            self.mark("head_loop", take, start)
            self.quiet()
        mid = top // 2
        # (full strokes from a little way out: at the very start the sled can knock against its stop)
        for label, a, b in (("head_short", mid, mid + 4000), ("head_medium", top // 4, 3 * top // 4),
                            ("head_full", 2000, top)):
            self.ops.drive.command(scsi.seek10(a))
            self.quiet(1.5)
            for take in range(1, 2 * self.takes + 1):
                start = self.now()
                self.ops.drive.command(scsi.seek10(b if take % 2 else a))
                self.wait(1.2)
                self.mark(label, take, start)
        self.ops.set_speed(None)

    def head_sequential(self):
        """The head creeping outwards as it does while reading a long file,
        with the disc slow and quiet: SEEKs 1,000 sectors apart, nonstop, 12 s
        a take. The synth's sequential.wav, without the full-speed spinning
        that reads bring (cut.py lays it over the idle loop)."""
        self.stop_and_settle()
        top = scsi.lba_at_radius(self.ops.max_radius()) - 100
        self.ops.drive.command(scsi.seek10(2000), timeout_s=30)  # the disc starts, slowly
        self.wait(3)
        lba = 2000
        for take in range(1, 3):
            start = self.now()
            end = time.perf_counter() + (1.2 if self.fast else 12)
            while time.perf_counter() < end:
                lba = lba + 1000 if lba + 1000 < top else 2000
                self.ops.drive.command(scsi.seek10(lba))
            self.mark("head_sequential", take, start)
            self.quiet()
        self.ops.set_speed(None)

    STEPS = ("room", "spinups", "left_alone", "speeds", "seeks", "access", "access_loops", "head",
             "head_sequential", "loading", "tray")

    def run(self, max_x: float, tray: bool, idle_timeout: float, only: list[str] | None = None) -> list[Event]:
        self.sync("start")
        steps = [("room", self.room), ("spinups", self.spinups),
                 ("left_alone", lambda: self.left_alone(idle_timeout or 45)), ("speeds", lambda: self.speeds(max_x)),
                 ("seeks", self.seeks), ("access", self.access), ("access_loops", self.access_loops),
                 ("head", self.head), ("head_sequential", self.head_sequential), ("loading", self.loading)]
        if tray:
            steps.append(("tray", self.tray))
        if only:
            steps = [s for s in steps if s[0] in only]
        for name, step in steps:
            print(f"{self.now():8.2f}  {name}")
            self.quiet(2)
            step()
        self.ops.set_speed(None)
        self.quiet(2)
        self.sync("end")
        return self.events


def _beep(hz: int, ms: int):
    import winsound
    winsound.Beep(hz, ms)


# ---- Recording from this PC (ffmpeg) -------------------------------------------------


def list_mics():
    p = subprocess.run(["ffmpeg", "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
                       capture_output=True, text=True)
    for line in p.stderr.splitlines():
        if "(audio)" in line:
            print(line.split("]", 1)[-1].strip())


def start_recording(mic: str, path: Path, rate: int) -> subprocess.Popen:
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "dshow", "-audio_buffer_size", "50",
           "-i", f"audio={mic}", "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", str(path)]
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


# ---- A drive for dry runs ------------------------------------------------------------


class FakeDrive:
    """Answers every command as a drive would, with plausible delays."""

    def __init__(self, sectors: int = 300_000, fast: bool = False):
        self.sectors, self.scale = sectors, 0.1 if fast else 1.0

    def command(self, cdb: bytes, data_in: int = 0, data_out=None, timeout_s: int = 30):
        from scsi import Result
        op = cdb[0]
        delay = {0x1B: 1.5 if cdb[4] & 1 else 0.2, 0x28: 0.08, 0x2B: 0.08}.get(op, 0.001)
        time.sleep(delay * self.scale)
        data = b""
        if op == 0x25:
            data = (self.sectors - 1).to_bytes(4, "big") + (2048).to_bytes(4, "big")
        elif op == 0x12:
            data = bytes(8) + b"FAKE    " + b"CD-ROM DRIVE    " + b"1.00"
        elif op == 0x5A:
            page = bytearray(28)
            page[0], page[1], page[6] = 0x2A, 26, 0x29  # tray, eject
            page[8:10] = (7056).to_bytes(2, "big")    # 40x
            page[14:16] = (7056).to_bytes(2, "big")
            data = bytes(8) + bytes(page)
        return Result(True, 0, data.ljust(data_in, b"\0")[:data_in] if data_in else b"", None, delay * self.scale)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drive", help="\\\\.\\CdRomN or a drive letter (default: the only CD drive)")
    ap.add_argument("--mic", help="record on this PC from this microphone (ffmpeg's dshow name)")
    ap.add_argument("--list-mics", action="store_true")
    ap.add_argument("--rate", type=int, default=48000, help="recording sample rate (default 48000)")
    ap.add_argument("--takes", type=int, default=4, help="takes of each sound (default 4)")
    ap.add_argument("--quiet", type=float, default=3.0, help="seconds of quiet between sounds (default 3)")
    ap.add_argument("--tray", action="store_true", help="record the tray opening and closing (and the disc loading)")
    ap.add_argument("--idle-timeout", type=float, default=45, help="how long to leave the drive alone for its own slow-down (default 45 s)")
    ap.add_argument("--steps", help="only these steps, comma-separated: " + ", ".join(Session.STEPS))
    ap.add_argument("--dry-run", action="store_true", help="a fake drive: check the plan and the log")
    ap.add_argument("--fast", action="store_true", help="with --dry-run: every wait a tenth as long")
    a = ap.parse_args()
    if a.list_mics:
        list_mics()
        return

    runs = HERE / "runs"
    runs.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = runs / f"session-{stamp}.json"
    if a.dry_run:
        drive, beep = FakeDrive(fast=a.fast), (lambda hz, ms: time.sleep(ms / 1000))
    else:
        from probe import pick_drive
        from scsi_win import CdDrive
        drive, beep = CdDrive(pick_drive(a.drive)), _beep
    ops = Ops(drive)
    info = ops.info()
    if not info.sectors:
        sys.exit("no data disc: put one in (python probe.py first)")
    print(f"{info.name}: {info.sectors:,} sectors, data out to {ops.max_radius():.1f} mm, fastest {info.max_x:.0f}x")
    session = Session(ops, a.quiet, a.takes, fast=a.fast, beep=beep)

    recorder = None
    if a.mic and not a.dry_run:
        recorder = start_recording(a.mic, log_path.with_suffix(".wav"), a.rate)
        print(f"recording to {log_path.with_suffix('.wav')}")
    else:
        input("Start your recorder, then press Enter (beeps first) ...") if not a.dry_run else None
    try:
        events = session.run(info.max_x or 8, a.tray or bool(a.steps and "tray" in a.steps), a.idle_timeout,
                             a.steps.split(",") if a.steps else None)
    finally:
        if recorder:
            stop_recording(recorder)
        drive_close = getattr(drive, "close", None)
        if drive_close:
            drive_close()
    log = {"drive": info.name, "sectors": info.sectors, "max_x": round(info.max_x, 1), "tray": info.tray,
           "max_radius_mm": round(ops.max_radius(), 1), "beep_hz": BEEP_HZ, "beep_ms": BEEP_MS,
           "beep_gap_s": BEEP_GAP_S, "recording": log_path.with_suffix(".wav").name if recorder else None,
           "dry_run": a.dry_run, "events": [asdict(e) for e in events]}
    log_path.write_text(json.dumps(log, indent=1))
    print(f"\n{len(events)} events, {events[-1].end:.0f} s: {log_path}")
    if not recorder:
        print("Stop your recorder; then: python cut.py <recording> " + str(log_path))


if __name__ == "__main__":
    main()
