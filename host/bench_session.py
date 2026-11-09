#!/usr/bin/env python3
"""Runs the rig half of a bench session (docs/bench-checklist.md) by itself:
for each step it flashes the build the step needs, sets the console up (test
disk, bus program, settings), makes the adapter detect the drive again, runs
the tests, reads the drive's stats, and puts the results beside what they
were last time. At the end it puts the settings it changed back.

Windows rig, from an administrator Git Bash in host/:

    export PYTHONUTF8=1 HDDPRO_DISK=PhysicalDrive1
    python bench_session.py --list               # the steps
    python bench_session.py                      # the default steps, in order
    python bench_session.py --only hdcd-speed,hdcd-cdrom
    python bench_session.py --from hdcd-speed    # carry on after a stop
    python bench_session.py --dry-run            # what it would do

Results go to runs/bench-<time>/: each step's pytest output, the drive's
stats, the serial log, and summary.md (also printed at the end).

It needs the adapter on USB (replug it first; restart the PC if it acts up)
and the board running any drive build. It never formats the card: the only
file it changes is HDDSYNTH.INI, through console E, and only the settings the
steps name (device0, device1, mode), which it restores; each step starts
from the settings as they were, plus its own. The retro PCs, MSCDEX and the
soak's long run stay by hand.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

HOST = Path(__file__).resolve().parent
REPO = HOST.parent
sys.path.insert(0, str(HOST))

from hddproto.drive_log import DriveLog  # noqa: E402
from hddproto.host import make_host  # noqa: E402

RIG_FLAGS = "-DRIG_WATCHDOG_MS=8000"  # the rig-only watchdog: a hung board restarts itself
TEST_DISC_FRAMES = 32768  # the CD-ROM's test disc (src/test_iso)
ADAPTER_IDS = ("USB\\VID_152D&PID_2338",)  # the adapter as Windows lists it (JMicron IDs)


@dataclass
class Step:
    key: str
    title: str
    env: str  # the PlatformIO build
    flags: str = ""  # build flags beyond RIG_FLAGS
    program: str | None = None  # console P: e (early answers), l (data lane), i (ide_bus)
    settings: dict[str, str] = field(default_factory=dict)  # console E, then !
    marker: str | None = None  # pytest -m
    cdrom: bool = False  # the test reads the CD-ROM (HDDPRO_CDROM_DEV)
    interleave: bool = False  # read the CD-ROM in the background meanwhile
    discs: bool = False  # change discs (console M) and watch Windows
    expect: dict[str, float] = field(default_factory=dict)  # last time's figures
    note: str = ""
    default: bool = True


HDCD = {"device1": "cdrom", "mode": "mdma2"}  # a hard disk, and a CD-ROM as device 1
CD0 = {"device0": "cdrom", "mode": "mdma2"}  # a CD-ROM drive alone, as device 0
STEPS = [
    Step("pio-lane", "PIO speed on the data lane, after dev.3's DMA fix", "bb48_drive", program="l",
         marker="speed", expect={"read_mbs": 1.54, "iops": 80, "write_mbs": 1.68}, note="checklist 1, dev.2's figures"),
    Step("pio-early", "PIO speed on early answers", "bb48_drive", program="e",
         marker="speed", expect={"read_mbs": 1.21, "iops": 76, "write_mbs": 1.13}, note="checklist 1, dev.2's figures"),
    Step("slices-lane", "Slices 3-4 on the data lane", "bb48_drive", program="l", marker="slice3 or slice4"),
    Step("hdcd-speed", "The hard disk (device 0) in DMA beside a CD-ROM", "bb48_drive", settings=HDCD,
         marker="speed", expect={"read_mbs": 8.84, "iops": 1000, "write_mbs": 12.48},
         note="checklist 2: bb48_drive's DMA on early answers; within noise means hdcd costs nothing"),
    Step("hdcd-cdrom", "Every byte of the test disc (device 1), with read-ahead", "bb48_drive",
         settings=HDCD, marker="cdrom", cdrom=True, expect={"cd_mbs": 2.42},
         note="checklist 2: dev.3 without read-ahead"),
    Step("hdcd-both", "Disk speed while the CD-ROM (device 1) is read", "bb48_drive", settings=HDCD,
         marker="speed", interleave=True, note="checklist 2: no errors, bus_bad=0"),
    Step("hdcd-discs", "Disc changes on device 1 seen by Windows (console M)", "bb48_drive", settings=HDCD,
         discs=True, note="checklist 2; MSCDEX on the Evesham stays by hand"),
    Step("cd-profile", "A CD-ROM as device 0: slow reads and command times, early answers", "bb48_drive",
         flags="-DCMD_PROFILE=1", program="e", settings=CD0, marker="cdrom", cdrom=True,
         note="checklist 2b"),
    Step("cd-profile-idebus", "A CD-ROM as device 0: the same on ide_bus", "bb48_drive", flags="-DCMD_PROFILE=1",
         program="i", settings=CD0, marker="cdrom", cdrom=True, note="checklist 2b"),
    Step("cd-pollfirst", "A CD-ROM as device 0: the candidate fix, EARLY_POLL_FIRST_NS=1000", "bb48_drive",
         flags="-DCMD_PROFILE=1 -DEARLY_POLL_FIRST_NS=1000", program="e", settings=CD0,
         marker="cdrom", cdrom=True, note="checklist 2b: fewer reads past 1,250 ns, no errors?"),
    Step("hdcd-soak", "A 60-minute soak while the CD-ROM (device 1) is read", "bb48_drive", settings=HDCD,
         marker="slice6", interleave=True, note="checklist 3 (not in the default run: --only hdcd-soak)", default=False),
]
STATS_KEPT = ("bus", "iordy_max_ns", "iordy_max_reg", "iordy_slow", "iordy_ignored", "bus_sectors", "bus_bad",
              "bus_gaps", "dior_no_cs", "cmd_run_max_ns", "cmd_run_max", "commands")
SOAK_ENV = {"HDDPRO_SOAK_MINUTES": "60", "HDDPRO_SOAK_TOLERATE_RESETS": "1", "HDDPRO_SOAK_IORDY_MAX_NS": "2000"}


def find_pio() -> str:
    for candidate in (shutil.which("pio"), Path.home() / ".platformio/penv/Scripts/pio.exe",
                      Path.home() / ".platformio-venv/bin/pio"):
        if candidate and Path(candidate).exists():
            return str(candidate)
    sys.exit("pio not found (rig.sh looks in the same places)")


def powershell(command: str) -> str:
    r = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True)
    return r.stdout.strip()


class Session:
    def __init__(self, out: Path, cd_dev: str, dry: bool, log):
        self.out, self.cd_dev, self.dry, self.log = out, cd_dev, dry, log
        self.drive: DriveLog | None = None
        self.flashed: tuple[str, str] | None = None
        self.program: str | None = None
        self.changed: dict[str, str] = {}  # setting: its value before this session
        self.settings: dict[str, str] = {}  # setting: its value now
        self.originals_path = out / "settings-before.json"

    # ---- the serial console ----

    def open(self, timeout: float = 40):
        self.drive = DriveLog(raw_log_path=self.out / "serial.log")
        self.drive.open(timeout=timeout)
        for _ in range(10):
            try:
                self.drive.ready = self.drive.command("h", "READY", timeout=2)
                return
            except TimeoutError:
                time.sleep(1)
        raise RuntimeError("no READY line from the board")

    def close(self):
        if self.drive:
            self.drive.close()
            self.drive = None

    def restart_with(self, keys: str):
        """Sends a console command that restarts the board, and waits for it."""
        self.drive.send(keys)
        time.sleep(1.5)
        self.close()
        time.sleep(2)
        self.open()

    def stats(self) -> dict[str, str]:
        return dict(re.findall(r"(\w+)=(\S+)", self.drive.command("s", "STATS", timeout=5)))

    def config(self) -> list[str]:
        mark = self.drive.mark()
        self.drive.command("S", "CONFIG seek_timing", timeout=15)
        return [l for l in self.drive.lines_since(mark) if l.startswith("CONFIG")]

    # ---- setting a step up ----

    def flash(self, env: str, flags: str):
        build_flags = f"{RIG_FLAGS} {flags}".strip()
        self.log(f"flash {env} ({build_flags})")
        if self.dry:
            return
        self.close()
        e = dict(os.environ, PLATFORMIO_BUILD_FLAGS=build_flags)
        with open(self.out / f"flash-{env}.log", "w", encoding="utf-8") as f:
            r = subprocess.run([find_pio(), "run", "-e", env, "-t", "upload"], cwd=REPO, env=e,
                               stdout=f, stderr=subprocess.STDOUT)
        if r.returncode:
            raise RuntimeError(f"flashing {env} failed: see flash-{env}.log")
        time.sleep(3)
        self.open(timeout=60)
        self.program = None  # a flash is a power-on: P and T are cleared
        self.log(f"  {self.drive.ready}")

    def set_settings(self, wanted: dict[str, str]):
        todo = {k: v for k, v in wanted.items() if self.settings.get(k) != v}
        if not todo:
            return
        for key, value in todo.items():
            self.log(f"E {key} = {value}")
            if self.dry:
                continue
            if key not in self.changed:
                self.changed[key] = self.settings.get(key, "")
                self.originals_path.write_text(json.dumps(self.changed, indent=1))
            self.drive.command("E", "# type", timeout=3)  # E's prompt
            reply = self.drive.command(f"{key} = {value}\r", "# ", timeout=5)  # "# saved ..." or "# E: ..."
            if "saved" not in reply:
                raise RuntimeError(f"E {key} = {value}: {reply}")
            self.settings[key] = value
        if not self.dry:
            self.restart_with("!")

    def read_settings(self):
        """device0, device1 and mode as HDDSYNTH.INI has them now (console S)."""
        if self.dry:
            return
        for line in self.config():
            if m := re.match(r"CONFIG file=\S+ device0=(\S+) mode=(\S+)", line):
                self.settings["device0"] = m.group(1)
                self.settings["mode"] = m.group(2)
            if m := re.match(r"CONFIG device1=(\S+)", line):
                self.settings["device1"] = m.group(1)
        self.settings.setdefault("device1", "none")
        self.log(f"settings now: {self.settings}")

    def test_disk(self) -> bool:
        cd0 = [l for l in self.config() if re.match(r"CONFIG device0=\S+ kind=cdrom", l)]
        if cd0:  # a CD-ROM as device 0: the test disc
            return f"frames={TEST_DISC_FRAMES}" in cd0[0]
        return self.stats().get("disk") == "generated"

    def ensure_test_disk(self):
        self.log("test disk (console T)")
        if self.dry or self.test_disk():
            return
        self.restart_with("T")
        if not self.test_disk():
            raise RuntimeError("console T didn't give the test disk")

    def set_program(self, program: str | None):
        """Console P; with none, P 0 (the settings' program), so an earlier
        step's choice doesn't carry over. Unknown after a flash."""
        program = program or "0"
        if program != self.program:
            self.log(f"P {program}")
            if not self.dry:
                self.restart_with("P" + program)
            self.program = program

    def redetect(self, cdrom_only: bool):
        """Console V, a read the adapter fails on, and the drive back at its
        next RESET#; then Windows restarts the adapter so it sees every drive."""
        self.log("re-detect")
        if self.dry:
            return
        target = self.cd_dev if cdrom_only else "//./" + os.environ.get("HDDPRO_DISK", "PhysicalDrive1")
        mark = self.drive.mark()
        if self.drive.command("V", "ABSENT", timeout=3) != "ABSENT on":
            raise RuntimeError("the drive didn't leave the bus")

        def poke():
            time.sleep(1)
            try:
                fd = os.open(target, os.O_RDONLY | getattr(os, "O_BINARY", 0))
                try:
                    os.lseek(fd, 64 * 2048, 0)
                    os.read(fd, 2048)
                finally:
                    os.close(fd)
            except OSError:
                pass

        threading.Thread(target=poke, daemon=True).start()
        try:
            self.drive.wait_for(lambda l: l.startswith("ABSENT off"), mark, 115)
            self.drive.wait_for(lambda l: l.startswith("CMD") and len(l.split()) > 2 and l.split()[2] in ("ec", "a1"),
                                mark, 60)
        except TimeoutError:
            self.log("  no detection seen: replug the adapter if the next step fails")
        time.sleep(3)
        if sys.platform == "win32":
            for dev in ADAPTER_IDS:
                ids = powershell(f"Get-PnpDevice -PresentOnly | Where-Object {{ $_.InstanceId -like '{dev}*' }} | "
                                 "ForEach-Object { $_.InstanceId }")
                for instance in ids.splitlines():
                    subprocess.run(["pnputil", "/restart-device", instance], capture_output=True)
            time.sleep(20)
            self.log("  " + powershell("(Get-Disk | Where-Object BusType -eq 'USB' | ForEach-Object "
                                       "{ \"disk $($_.Number) $($_.FriendlyName) $($_.Size)\" }) -join '; '")
                     + " | " + cd_state())

    # ---- running a step ----

    def pytest(self, step: Step) -> tuple[str, str]:
        env = dict(os.environ, PYTHONUTF8="1")
        if step.cdrom:
            env["HDDPRO_CDROM_DEV"] = self.cd_dev
        if step.marker == "slice6":
            env.update(SOAK_ENV)
        cmd = [sys.executable, "-m", "pytest", "-s", "-m", step.marker]
        self.log(f"pytest -m '{step.marker}'")
        if self.dry:
            return "", "dry run"
        reader = None
        if step.interleave:  # the test disc, whose size the reader wraps at
            self.drive.send("M")
            time.sleep(0.3)
            mark = self.drive.mark()
            self.drive.send("test\r")
            try:
                self.log("  " + self.drive.wait_for(lambda l: l.startswith("DISC "), mark, 30))
            except TimeoutError:
                self.log("  M test: no DISC line")
            time.sleep(5)  # Windows sees the new disc
            reader = CdReader(self.cd_dev, TEST_DISC_FRAMES * 2048)
        self.close()  # pytest opens the port itself
        if reader:
            reader.start()
        lines = []
        with subprocess.Popen(cmd, cwd=HOST, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding="utf-8", errors="replace") as p:
            for line in p.stdout:
                print("    " + line.rstrip())
                lines.append(line)
        text = "".join(lines)
        (self.out / f"{step.key}.log").write_text(text, encoding="utf-8")
        if reader:
            reader.stop()
            text += f"\ncd reader: {reader.summary()}\n"
        self.open()
        summary = next((l.strip("= \n") for l in reversed(lines) if re.search(r"passed|failed|skipped|error", l)),
                       "no pytest summary")
        return text, summary

    def discs(self) -> list[str]:
        """Eject, the ISO the settings name, the test disc; Windows's view after each."""
        results = []
        iso = next((l.split("cdrom=")[1].split()[0] for l in self.config() if "cdrom=" in l), "DOOM_SE.ISO")
        for disc, loaded in (("none", False), (iso, True), ("test", True)):
            self.log(f"M {disc}")
            self.drive.send("M")
            time.sleep(0.3)
            mark = self.drive.mark()
            self.drive.send(disc + "\r")
            try:
                reply = self.drive.wait_for(lambda l: l.startswith("DISC "), mark, 30)
            except TimeoutError:
                reply = "no DISC line"
            seen = "?"
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                seen = cd_state()
                if ("media=True" in seen) == loaded:
                    break
                time.sleep(2)
            ok = ("media=True" in seen) == loaded
            results.append(f"M {disc}: {reply}; Windows: {seen} {'ok' if ok else 'NOT AS EXPECTED'}")
            self.log("  " + results[-1])
        return results


def cd_state() -> str:
    if sys.platform != "win32":
        return ""
    return powershell("Get-CimInstance Win32_CDROMDrive | ForEach-Object "
                      "{ \"cd $($_.Drive) media=$($_.MediaLoaded) volume=$($_.VolumeName)\" }") or "no CD drive"


class CdReader:
    """Reads the CD-ROM over and over in 64 KB requests, until stopped."""

    def __init__(self, path: str, size: int):
        self.path, self.size, self.bytes, self.errors, self.last_error = path, size, 0, 0, ""
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._start = 0.0

    def start(self):
        self._start = time.monotonic()
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=10)

    def _run(self):
        try:
            fd = os.open(self.path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
        except OSError as e:
            self.errors, self.last_error = 1, str(e)
            return
        offset = 0
        while not self._stop.is_set():
            try:
                os.lseek(fd, offset, 0)
                n = len(os.read(fd, 65536))
            except OSError as e:
                self.errors += 1
                self.last_error = str(e)
                n = 0
            if n < 65536 or offset + n >= self.size:  # the disc's end: from the start again
                offset = 0
            else:
                offset += n
            self.bytes += n
        os.close(fd)

    def summary(self) -> str:
        seconds = max(0.001, time.monotonic() - self._start)
        s = f"{self.bytes / 1e6:.1f} MB at {self.bytes / 1e6 / seconds:.2f} MB/s, {self.errors} errors"
        return s + (f" (last: {self.last_error})" if self.errors else "")


def figures(text: str) -> dict[str, float]:
    """The numbers the speed and CD-ROM tests print."""
    found = {}
    for key, pattern in (("read_mbs", r"sequential read\s+([\d.]+) MB/s"), ("iops", r"random 4 KB read\s+([\d.]+) IOPS"),
                         ("write_mbs", r"sequential write\s+([\d.]+) MB/s"), ("worst_ns", r"worst read (\d+) ns"),
                         ("cd_mbs", r"Read \d+ MB in [\d.]+ s \(([\d.]+) MB/s\)")):
        if m := re.search(pattern, text):
            found[key] = float(m.group(1))
    return found


NAMES = {"read_mbs": "reads MB/s", "iops": "random 4 KB IOPS", "write_mbs": "writes MB/s", "cd_mbs": "CD-ROM MB/s",
         "worst_ns": "worst read ns (traffic)"}


def report(step: Step, summary: str, found: dict[str, float], stats: dict[str, str], extra: list[str]) -> str:
    out = [f"## {step.key}: {step.title}", "", f"pytest: {summary}" if step.marker else "", ""]
    rows = [(NAMES[k], f"{v:g}", f"{step.expect[k]:g}" if k in step.expect else "",
             f"{(v / step.expect[k] - 1) * 100:+.0f}%" if step.expect.get(k) else "") for k, v in found.items()]
    if rows:
        out += ["| | now | before | change |", "|---|---|---|---|"] + [f"| {a} | {b} | {c} | {d} |" for a, b, c, d in rows]
        out.append("")
    if step.note:
        out += [f"*{step.note}*", ""]
    if stats:
        out += ["`" + " ".join(f"{k}={stats[k]}" for k in STATS_KEPT if k in stats) + "`", ""]
    out += [f"- {e}" for e in extra]
    return "\n".join(l for l in out if l is not None) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list the steps and stop")
    ap.add_argument("--only", help="comma-separated steps to run")
    ap.add_argument("--from", dest="start", help="start at this step (the default steps from there)")
    ap.add_argument("--dry-run", action="store_true", help="say what it would do, touching nothing")
    ap.add_argument("--cd-dev", default="//./CdRom0", help="the CD-ROM's raw device (default //./CdRom0)")
    ap.add_argument("--keep-settings", action="store_true", help="don't restore HDDSYNTH.INI at the end")
    ap.add_argument("--as-is", action="store_true",
                    help="run the tests on the board as it is set up, and the adapter as it was plugged in: no "
                         "flash, console set-up or re-detection, each of which takes the drive off the bus "
                         "(the USB adapter hangs when a CD-ROM vanishes under it)")
    a = ap.parse_args()

    if a.list:
        for s in STEPS:
            print(f"{s.key:18} {'' if s.default else '(optional) '}{s.title}")
        return
    steps = [s for s in STEPS if s.default]
    if a.only:
        wanted = a.only.split(",")
        unknown = set(wanted) - {s.key for s in STEPS}
        if unknown:
            sys.exit(f"no such step: {', '.join(sorted(unknown))} (--list)")
        steps = [s for s in STEPS if s.key in wanted]
    elif a.start:
        keys = [s.key for s in steps]
        if a.start not in keys:
            sys.exit(f"no such default step: {a.start} (--list)")
        steps = steps[keys.index(a.start):]

    out = HOST / "runs" / f"bench-{datetime.now():%Y%m%d-%H%M%S}"
    out.mkdir(parents=True, exist_ok=True)
    log_file = open(out / "session.log", "a", encoding="utf-8")

    def log(message: str):
        line = f"{datetime.now():%H:%M:%S} {message}"
        print(line, flush=True)
        log_file.write(line + "\n")
        log_file.flush()

    session = Session(out, a.cd_dev, a.dry_run, log)
    sections = [f"# Bench session {datetime.now():%d %b %Y %H:%M}", ""]
    if not a.dry_run:
        session.open()
        log(session.drive.ready)
        session.read_settings()
    try:
        for step in steps:
            log(f"== {step.key}: {step.title}")
            started = time.monotonic()
            try:
                if a.as_is:
                    log("as it is: no flash, settings, test disk, bus program or re-detection")
                else:
                    if session.flashed != (step.env, step.flags):
                        session.flash(step.env, step.flags)
                        session.flashed = (step.env, step.flags)
                        session.read_settings()
                    # The settings as they were before the session, plus the step's own
                    session.set_settings({**{k: v or "none" for k, v in session.changed.items()}, **step.settings})
                    session.ensure_test_disk()
                    session.set_program(step.program)
                    session.redetect(cdrom_only=step.settings.get("device0") == "cdrom")
                if not a.dry_run:
                    session.drive.send("r")  # read timings from here
                text, summary = session.pytest(step) if step.marker else ("", "")
                extra = session.discs() if step.discs and not a.dry_run else []
                if m := re.search(r"cd reader: (.*)", text):
                    extra.append("CD-ROM read meanwhile: " + m.group(1))
                stats = {} if a.dry_run else session.stats()
                (out / f"{step.key}-stats.txt").write_text(json.dumps(stats, indent=1))
                section = report(step, summary, figures(text), stats, extra)
            except Exception as e:  # one step's failure doesn't stop the rest
                log(f"  step failed: {e}")
                section = f"## {step.key}: {step.title}\n\n**Didn't finish:** {e}\n"
                if session.drive is None and not a.dry_run:
                    try:
                        session.open()
                    except Exception as e2:
                        log(f"  board unreachable ({e2}); stopping")
                        sections.append(section)
                        break
            section += f"\n({(time.monotonic() - started) / 60:.0f} min)\n"
            sections.append(section)
            (out / "summary.md").write_text("\n".join(sections), encoding="utf-8")
    finally:
        if session.changed and not a.keep_settings and not a.dry_run:
            log(f"restoring HDDSYNTH.INI: {session.changed}")
            try:
                if session.drive is None:
                    session.open()
                session.set_settings({k: v or "none" for k, v in session.changed.items()})
            except Exception as e:
                log(f"  couldn't restore ({e}): set them with console E; they're in {session.originals_path}")
        session.close()
    sections.append("Afterwards: power-cycle the board to clear the console's T and P, and flash a normal build "
                    "(without RIG_WATCHDOG_MS) before it goes anywhere else.\n")
    (out / "summary.md").write_text("\n".join(sections), encoding="utf-8")
    print("\n" + "\n".join(sections))
    log(f"results in {out}")


if __name__ == "__main__":
    main()
