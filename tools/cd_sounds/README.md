# CD-ROM drive sounds: recording a sample pack

Tools to record a real CD-ROM drive's sounds and make an HDD Synth sample pack
of them. The drive is on the rig's USB-IDE adapter; the PC drives it with SCSI
commands (spin up and down, the tray, seeks, reads at each speed) while a
microphone records, then the recording is cut into a pack by the session's log.

A CD-ROM pack is a hard disk's pack (`spinup.wav`, `idle.wav`, `access.wav`,
`spindown.wav`, and `sequential.wav`) plus `tray_open.wav` and
`tray_close.wav`. The HDD Synth plays it beside the disk's pack, from what the
Bay says its CD-ROM drive is doing (`$HDO`,
[activity-link.md](../../docs/activity-link.md#protocol-version-3-the-cd-rom-drives-sounds-hdo)):
copy `packs/<name>/` (only the pack's files and `pack.json`) to `cdrom/<name>/`
on the synth's card. The rest (`extras/`, `takes/`) is for listening and for
simulations.

**Access and sequential are the head alone.** At full speed the disc's rush
buries the head (the LG's seeks stand 6-8 dB over it at best), and a loop
recorded while reading is mostly spinning: laid over `idle.wav` by the synth,
two spindle hums beat together (a slow pulsing). So the session also records
the head with the disc stopped (SEEK only, no reads: the LG then turns the
disc only slowly, and its head stands about 25 dB clear), and `cut.py` builds
`access.wav` from those seeks (half short, a third medium, a few full strokes,
at random gaps) and `sequential.wav` from the head stepping outwards, both
high-passed at 100 Hz and level with `idle.wav` (`--access-boost`; the synth's
balance turns them up or down from there).

Windows or macOS, Python 3.10 or later, the standard library and ffmpeg (on
the Windows PC from scoop; on the Mac from Homebrew). No numpy.

| Script | Does |
|---|---|
| `probe.py` | Which drive, which disc, and which commands reach the drive |
| `session.py` | The recording session: every sound a few times, logged against two sync beeps |
| `cut.py` | The recording and the log into `packs/<name>/` |
| `preview.py` | The pack in scenarios (the synth, a game, an install, CD audio), played or saved |
| `listen.py` | Recording from this PC's microphone (ffmpeg), and the spindle's tone (its speed) |
| `placement.py` | Where to put the microphone: 20 s of seeks and reads against spinning, in dB, logged per position |
| `scsi.py`, `drive_ops.py`, `audio.py` | The commands, the drive's sounds as operations, WAV editing |
| `platform_io.py`, `scsi_win.py`, `scsi_mac.py` + `scsi_mac.c` | What differs by OS: the drive (Windows' SCSI pass-through; on macOS IOKit's MMC interface, by a helper in C), the beep, the microphone |

## Setting up

- **The drive:** an IDE CD-ROM drive, jumpered as master (or cable select on the
  adapter's end), on the adapter's 40-pin header, powered from the adapter's
  brick (its Molex lead). The adapter shows as USB `152D:2338`, and Windows
  gives the drive a letter.
- **A disc:** a data CD, as full as you have (a pressed game or install CD is
  ideal): seeks only reach as far out as its data does. `probe.py` says how
  far (58 mm is a full disc).
- **AutoPlay off** (Settings, Bluetooth & devices, AutoPlay), so Windows
  doesn't spin the drive up between sounds. Windows still polls the drive
  about once a second, which is quiet once the disc is known.
- **An administrator terminal** (Git Bash or PowerShell): the SCSI
  pass-through needs it.
- **On the Mac** instead (first written 22 Nov 2026 and not yet run on one):
  - Xcode's command line tools (`xcode-select --install`): the first run
    builds `runs/scsi_mac` from `scsi_mac.c` with clang. No `sudo` needed.
  - `brew install ffmpeg`; the Terminal asks for the microphone the first time.
  - The adapter shows as `2537:1081` there. macOS hands the drive over only
    while its disc isn't mounted: the scripts unmount it (`diskutil
    unmountDisk`) and macOS mounts it again when they finish. If `probe.py`
    says the disc is still mounted, quit whatever has it open (the Finder
    window, Spotlight indexing it) and try again.
  - `python3 probe.py` lists the drives by number (and `diskN`):
    `--drive 0` or `--drive disk4` picks one.
  - `--list-mics` gives AVFoundation's names (`Samson Meteor Mic`, not
    Windows' `Microphone (Samson Meteor Mic)`). The beeps play through the
    Mac's output (`afplay`).
  - Keep the Mac awake for the session (`caffeinate -dimsu python3 session.py
    ...`).
- **The microphone:** close to the drive (10-20 cm, toward the tray and the
  side where the motor is), in a quiet room, nothing touching the drive. A
  bare drive on a desk sounds thinner than one in a case: for a PC's sound,
  stand it on a hollow box, or mount it in a case with the lid off.
  - On this PC: a USB microphone, then `python session.py --list-mics` for
    its name, and `--mic "NAME"`: the session records it itself.
  - Or your own recorder (a phone, a Zoom): start it before the session; it
    must hear the PC's two beeps at the start and the end (speakers up).
    Any format ffmpeg reads; 48 kHz, 24-bit is plenty.

## Recording

From an administrator terminal in `tools/cd_sounds` (on the Mac, any Terminal, with `python3`):

```
python probe.py                  # the drive, the disc, what works (about 20 s)
python probe.py --tray           # the tray too
python session.py --dry-run --fast --tray   # the plan, with a fake drive (no sound)
python session.py --tray --mic "Microphone (USB Audio)"
```

The session (about 15 minutes), its steps in order (`--steps a,b` runs only
some: a second session can redo the head, say, and `cut.py` takes both):

1. Two beeps (the sync); `room`: the room's noise with the disc stopped.
2. `spinups`: four takes of spin up from rest (reading), ten seconds spinning,
   spin down.
3. `left_alone`: a read, then nothing, until the drive slows by itself
   (`--idle-timeout`, 45 s; the LG brakes to a slow idle after 30 s).
4. `speeds`: reading at each speed the drive obeys, and spinning after it.
5. `seeks`: single seeks at full speed, short (0.6 mm), medium (12 mm) and full.
6. `access`, `access_loops`: bursts of random seeks with short reads, and 12 s
   of them nonstop (at full and medium speed).
7. `head`: the disc stopped, then SEEK only: loops, and single short, medium
   and full seeks (full strokes from a little way out, so the sled doesn't
   knock its stop); `head_sequential`: SEEK 1,000 sectors on, nonstop, 12 s.
8. `loading`: a seek and a few seconds of reading, three times.
9. With `--tray`: the tray out, then in (the disc clamped, spun up, its table
   of contents read), four takes.
10. Two beeps.

Keep quiet while it runs: every sound has a few seconds of quiet around it,
but a cough in a take is a take lost (`cut.py` picks among four).

The log goes to `runs/session-<time>.json` (and the recording beside it with
`--mic`).

## Making the pack

```
python cut.py runs/session-<time>.json --name CD_Mitsumi_FX400
python cut.py runs/session-A.json runs/session-B.json --name CD_LG_GCC4320B --pick spinning=3
python preview.py packs/CD_Mitsumi_FX400          # every scenario
python preview.py packs/CD_Mitsumi_FX400 game --save game.wav
```

A session recorded on this PC (`--mic`) has its recording beside its log; for
another recorder's file give `LOG=RECORDING`. With several sessions, each
later one's sounds replace the earlier's (the LG's pack: the full session,
then one redoing the room, the head and the sequential steps).

`cut.py` finds the beeps (give `--offset SECONDS` if it can't: where the first
starts), corrects for the recorder's clock from the end beeps, and writes
`packs/<name>/`: the pack's files, `pack.json` (its `NAME`, for the synth,
which reads only 1 KB of it; add `GAIN` or `AUTHOR` by hand), `cut.json`
(where each file came from, the gain, the clocks), `extras/` and every take
in `takes/`. It picks the median take of each sound by loudness; listen to
`takes/` and choose others with `--pick spinup=3 --pick spinning=1`. The
whole pack shares one gain, so the sounds keep their loudness against each
other.

- **16 kHz** by default (`--rate`): the synth plays the disk's pack and the
  CD-ROM's at one rate, and every HDD pack so far is 16 kHz.
- **The tray:** `tray_open.wav` is the tray coming out after the brake, and
  `tray_close.wav` the tray going in and the disc clamped, before it spins
  up: the synth plays `spindown.wav` before the one and `spinup.wav` after
  the other.
- **Access and sequential** level with idle (`--access-boost 0`); +6 dB was
  easy to hear but too prominent.
- Seeks from a take with a noisy background (over 4 dB above the typical) or
  a loud outlier (over 6 dB above) are left out of `access.wav`.

## What may not work

- **SET CD SPEED** is the command an adapter or an old drive most often
  refuses, or accepts and ignores: `probe.py` measures the read rate at 4x
  against the fastest, and the session then records the drive at its own
  speed only. No 1x recording for CD audio then. The LG GCC-4320B (a DVD/CD-RW
  combo, 21 Nov 2026) accepts it and ignores it, and refuses SET STREAMING:
  it spins at one speed (constant angular velocity: about 15x at the inner
  edge, 28x at the outer).
- **Spin down** on command: some drives ignore it, or spin up again at
  Windows' next poll. The drive's own idle spin-down (`--idle-timeout`) still
  gives one.
- **The adapter declines** after long use (`CLAUDE.md`, *Rig*): if commands
  start failing, replug it, or restart the PC.

## Tests

`python -m pytest -q` here: the commands and answers, finding the beeps in
noise, the loops, and a synthetic session cut into a pack with every scenario
rendered (no drive or microphone needed; ffmpeg for that last one).
