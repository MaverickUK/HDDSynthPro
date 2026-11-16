# Synth Link: bringing IDE Synth Bay and the HDD Synth together

**Status (16 Nov 2026): a plan, from the desk. Nothing here has been run with a synth.** The protocol and both ends' code are in [activity-link.md](activity-link.md); this note is the order to bring them up in, what must be fixed first, and the design for the realism settings (a slower, period-correct drive).

## Where it stands

| Piece | State |
|---|---|
| IDE Synth Bay sends `$HDA` (busy, class, operation) | built (0.9.0), sends on the rig; never received by a synth |
| IDE Synth Bay sends `$HDE` (where the head goes) | built (0.19.0), host-tested only |
| IDE Synth Bay reads the synth's `$HDM` (its drive model) | built (0.19.0), host-tested only |
| Seek timing (`seek_timing`: the drive held to a modelled drive's pace) | built (0.19.0), host-tested; `-m seek` on the rig |
| The loopback test (console `U`, a jumper GPIO 0 to GPIO 1) | built (0.10.1); never run with the jumper |
| HDD Synth: the receiver, `sequential.wav`, modelled drives, `$HDM` | built (HDDSynth_Native v0.18.0-v0.21.0; now v0.24.0) |

So the work is bring-up and what it finds, not new protocol, except for the realism settings below.

**Which packs.** A recorded pack (WAVs) can use only version 1: busy and the class, which pick `access.wav` or `sequential.wav`. Everything that reacts to what the emulated drive does (the head moving to where the command is, short seeks ticking and long ones clunking, the drive's pace matching its sound) needs a **modelled pack** (`drive.json`), because only those have a head to move and a model to send back. Bring-up uses one of the synth's presets as a `drive.json` pack: `voice97` first (an ordinary 1997 drive), then `stepper85` (the slowest, where timing errors are easiest to hear).

## Before any wire is connected

1. **Bring the copied files up to date.** `drive_params.h` and `drive_params.cpp` here differ from HDDSynth_Native's (copied at its v0.21.0, 6a68a71; it is at v0.24.0, with `drive.json` format version 2). `drive_timing.*` and `sentence.*` still match. Copy the two back, note the commit in the CHANGELOG, and run the native tests: seek timing must agree with what the synth plays.
2. **Check the known answers on both sides** (`$HDA`, `$HDE`, `$HDM` in activity-link.md) against HDDSynth_Native's tests at its current commit.
3. **Seek timing with what has changed since 0.19.0:** READ/WRITE MULTIPLE (a block is released when its last sector would be ready), the register table, the data lane and the card's high-speed mode. Host tests exist for the scheduler; run `-m seek` on each bus program on the rig.

## Bring-up, in order

Each step has a pass line, so a failure says which half is at fault.

1. **Loopback, no synth.** A jumper from GPIO 0 to GPIO 1, console `U` (`LOOPBACK pass ... head_valid= model=ok`), `pytest -m link`. Proves the UART, both directions, on this board.
2. **One wire: `$HDA`.** GPIO 0 through 1 kΩ to the synth's J6 pin 3, and ground (the wiring table in activity-link.md; never join the 3.3 V pins). On the synth: `EXPERIMENTAL_HDDSYNTHPRO_SUPPORT: true` (its OLED goes off: they share J6) and a recorded pack with a `sequential.wav`. Pass: the synth's `s` shows the link `up` with valid sentences and none rejected; a long sequential read (`-m activity`, or a file copy on the Evesham) switches it to the sequential sample and its LED colour, and random access back.
3. **A modelled pack: `$HDE`.** The same wire, the synth on a `voice97` `drive.json` pack. Pass: `s` here shows `head_sent` rising with media commands; on the synth, short seeks and long ones sound different (a read at the start of the disk, then the end, then back). Compare with the LED-only behaviour (link unplugged): the synth should fall back to guessing.
4. **The second wire: `$HDM`.** The synth's J6 pin 4 through 1 kΩ to GPIO 1. Pass: `s` shows `hdm_valid` rising every 2 s and `hdm_rejected` 0; `S` prints the model it heard (`CONFIG seek_timing=... model=voice 5400rpm ...`); changing the pack on the synth changes it here within 2 s; a recorded pack gives `$HDM,2,-` and nothing is held.
5. **`seek_timing = synth`.** Pass: `s` shows `seek_held` and `seek_avg_us` in line with the model (`voice97`: about 9 ms a random access plus the turn), `seek_capped` 0, and the sound of each seek ends about when the PC gets its data. On the Evesham: DOS boots, `DOSBENCH.IMG` passes with the drive at the modelled pace, and it sounds like the PC feels.
6. **The slow drive.** `stepper85` on both. The same checks; full-stroke seeks take half a second, so anything out of step is audible.
7. **Robustness.** The synth powered off and on mid-transfer (the link `lost` then `up`; with `synth`, holds stop within 5 s and resume); the synth unplugged; a pack change mid-transfer; an hour's soak with seek timing on (`HDDPRO_SOAK_IORDY_MAX_NS=2500` as usual); a CD-ROM beside the disk (its reads aren't modelled: check they aren't held).

**What to record:** both consoles' `s` before and after each step, and for step 5 a recording of the synth beside the Evesham's benchmark log, in `host/runs/`.

## Known gaps, to decide during bring-up

- **`SEEK` commands don't move the modelled head** (no fetch reaches core 0 for them), and a write's first sector is timed from when it arrives. Both are in activity-link.md; DOS uses neither much.
- **The CD-ROM isn't modelled or shown** (protocol version 3, [hddsynth-cdrom-display-brief.md](hddsynth-cdrom-display-brief.md)).
- **Standby and wake** aren't sent: the synth can't spin down when the host sends STANDBY. A small addition to `$HDA` or a new sentence; wanted for realism (below).
- **The synth's OLED and the link share J6** on the Mk5, so the link costs the display until the synth's next hardware revision.

## Realism settings: a period-correct drive

**The aim.** Let someone set the drive up to behave like an older mechanical one: its seek time and its transfer speeds, so it sounds and feels right, accepting that it is then slow.

**What exists.** `seek_timing` already holds each sector until a modelled drive would have it: the seek (from the model's stroke and track times), the wait for the sector to come round (from its rpm), and the transfer (the sectors passing under the head). The presets are whole drives: `stepper85`, `voice91`, `voice97`, `fdb05`. With `synth`, the model comes from the pack the HDD Synth is playing.

**What is missing.**
- **The transfer rate is fixed at 63 sectors a track**, whatever the drive. At 3,600 rpm that is 1.9 MB/s, where a 1985 MFM drive had 17 sectors a track and managed about 0.5 MB/s. So the old presets seek slowly but transfer too fast.
- **No way to set a number directly.** People know a drive by its data sheet: "28 ms average seek, 600 KB/s". Today the only choices are four presets or building a `drive.json` in the Drive Lab.
- **Reads and writes take the same time.** Fine for most drives; some people will want writes slower.
- **`$HDM` doesn't carry a transfer rate**, so `synth` can't follow a pack's.

**Proposed settings** (`HDDSYNTH.INI`; all optional, each overriding the model in use):

| Setting | Meaning | Default |
|---|---|---|
| `seek_timing` | as now: `off`, `synth`, a preset; plus `custom`: no preset, only the numbers below | `off` |
| `seek_ms` | the average seek time, in ms, as a data sheet gives it. The model's full-stroke and track-to-track times are scaled to give it | the model's |
| `read_rate` | the most the drive reads from the media, in KB/s | the model's |
| `write_rate` | the same for writes | `read_rate` |
| `rpm` | the spindle speed: sets the wait for a sector to come round | the model's |

- `seek_timing = custom` with `seek_ms = 28`, `read_rate = 600`, `rpm = 3600` is a complete drive, with no synth and no preset.
- With `seek_timing = off` the numbers do nothing, and `S` says so, so a setting left in the file can't slow the drive by surprise.
- **How the rate is applied:** as sectors per track (`rate / (512 x rpm / 60)`), which `transferUs()` and `rotationalWaitUs()` already take, so no new mechanism: a slower rate is fewer sectors passing per turn. Reads and writes each get their own figure.
- **The cap stays:** no command is held over a second (`seek_capped`), so a silly value can't hang a host.
- **`S` prints the drive in force** (`CONFIG seek_timing=custom model=... seek_avg=28.0ms read=600KB/s write=600KB/s`), and `s` keeps `seek_avg_us` and adds the media rate achieved.

**Where the truth lives when a synth is connected.** Two ends must agree, or the sound and the feel drift apart: the synth draws each seek's sound from its own model. Proposed rule:

- **With a synth, the pack is the truth.** `seek_timing = synth` follows the pack, including a transfer rate once `$HDM` carries one. The Drive Lab is where the drive is tuned, by ear.
- **`HDDSYNTH.INI`'s numbers are for a drive without a synth**, or deliberate overrides. If they override while a synth plays, `S` warns that the sound and the pace will differ (a seek that sounds 9 ms and takes 28).
- **Later, if wanted:** a sentence from here to the synth carrying the timing in force, so the synth stretches its seeks to match. Not in the first version.

**What it needs in HDDSynth_Native** (a brief, as the earlier ones: `docs/hddsynth-*-brief.md`):
- `drive.json`: the media's sectors per track (or read and write rates) in the Drive Lab and the format;
- `$HDM` version 3: the same as a trailing field. This side accepts version 2 (63 sectors a track, as now) and 3;
- its copies of the timing functions stay the shared source: change them there, copy them here.

**Work, in order:**
1. `seek_ms`, `read_rate`, `write_rate`, `rpm` and `custom` here (`drive_config`, `seek_timing`, `S` and `s`); host tests for the scaling and the rates; `-m seek` on the rig. No synth needed.
2. Correct the old presets' transfer rates (17 and 26 sectors a track for the 1985 and 1991 drives, in HDDSynth_Native, copied back).
3. The brief for `drive.json` and `$HDM` version 3; then `synth` follows the pack's rate.
4. Spin-down and spin-up over the link (STANDBY and wake), so the synth's spindle follows the drive's.

**Costs to know.** A held drive is slow by design: `stepper85` runs a DOS boot at a 1985 drive's pace. Holds are BSY, never IORDY, so they are safe on every host; a BIOS with a short timeout could give up on the slowest settings, which is what the one-second cap is for. Core 1 is untouched: all of this is core 0, releasing results later.
