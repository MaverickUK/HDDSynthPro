# CD-ROM drive sounds: recording a sample pack

Tools to record a real CD-ROM drive's sounds and make an HDD Synth sample pack
of them. The drive is on the rig's USB-IDE adapter; the PC drives it with SCSI
commands (spin up and down, the tray, seeks, reads at each speed) while a
microphone records, then the recording is cut into a pack by the session's log.

The pack has the four files today's HDD Synth plays (`spinup.wav`, `idle.wav`,
`access.wav`, `spindown.wav`, plus the optional `sequential.wav`), so it plays
on the synth as it is; the rest (`extras/`) is for a CD-ROM voice of its own
later (the Bay telling the synth what its CD-ROM drive is doing).

Windows, Python 3.10 or later, the standard library and ffmpeg (on this PC
from scoop). No numpy.

| Script | Does |
|---|---|
| `probe.py` | Which drive, which disc, and which commands reach the drive |
| `session.py` | The recording session: every sound a few times, logged against two sync beeps |
| `cut.py` | The recording and the log into `packs/<name>/` |
| `preview.py` | The pack in scenarios (the synth, a game, an install, CD audio), played or saved |
| `scsi.py`, `scsi_win.py`, `drive_ops.py`, `audio.py` | The commands, Windows' SCSI pass-through, the drive's sounds as operations, WAV editing |

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

From an administrator terminal in `tools/cd_sounds`:

```
python probe.py                  # the drive, the disc, what works (about 20 s)
python probe.py --tray           # the tray too
python session.py --dry-run --fast --tray   # the plan, with a fake drive (no sound)
python session.py --tray --mic "Microphone (USB Audio)"
```

The session (about 10 minutes; `--idle-timeout 180` adds the drive's own
spin-down after three minutes alone):

1. Two beeps (the sync), then the room's noise with the disc stopped.
2. Four takes of: spin up from rest, eight seconds spinning idle, spin down.
3. Reading at each speed up to the fastest (1x, 2x, 4x, ...), each followed by
   six seconds spinning idle at it.
4. Single seeks: short (0.6 mm), medium (12 mm), and full stroke, eight each.
5. Access bursts (six random seeks with short reads), and loading (a seek and
   two seconds of reading, three times), four takes each.
6. With `--tray`: the tray out, then in (the disc loading: spin up and the
   drive reading its table of contents), four takes.
7. Two beeps.

Keep quiet while it runs: every sound has a few seconds of quiet around it,
but a cough in a take is a take lost (`cut.py` picks among four).

The log goes to `runs/session-<time>.json` (and the recording beside it with
`--mic`).

## Making the pack

```
python cut.py runs/session-<time>.wav runs/session-<time>.json --name CD_Mitsumi_FX400
python preview.py packs/CD_Mitsumi_FX400          # every scenario
python preview.py packs/CD_Mitsumi_FX400 game     # one
```

`cut.py` finds the beeps (give `--offset SECONDS` if it can't: where the first
starts), corrects for the recorder's clock from the end beeps, and writes
`packs/<name>/`: the pack's files, `extras/`, every take in `takes/`, and
`pack.json` (where each file came from). It picks the median take of each
sound by loudness; listen to `takes/` and choose others with
`--pick spinup=3 --pick access=1`. The whole pack shares one gain, so the
sounds keep their loudness against each other.

To play it on the HDD Synth: copy `packs/<name>/` (without `takes/`) into
`samples/` on its SD card, as any pack (HDDSynth_Samples' README).

## What may not work

- **SET CD SPEED** is the command an adapter or an old drive most often
  refuses (`probe.py` says). Without it every read is at the drive's own
  speed, and there's no 1x recording for CD audio.
- **Spin down** on command: some drives ignore it, or spin up again at
  Windows' next poll. The drive's own idle spin-down (`--idle-timeout`) still
  gives one.
- **The adapter declines** after long use (`CLAUDE.md`, *Rig*): if commands
  start failing, replug it, or restart the PC.

## Tests

`python -m pytest -q` here: the commands and answers, finding the beeps in
noise, the loops, and a synthetic session cut into a pack with every scenario
rendered (no drive or microphone needed; ffmpeg for that last one).
