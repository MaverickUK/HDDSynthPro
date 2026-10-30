# Brief: head positions for modelled drives, and authentic seek timing

> **Built in HDDSynthPro 0.19.0** ([activity-link.md](activity-link.md#protocol-version-2-head-positions-and-the-synths-model)). Where it differs from this brief: a command that carries straight on from the last one's next sector doesn't seek or wait for the platter (a drive's read-ahead), and `SEEK` doesn't move the modelled head yet. Not yet run on the rig or with the synth.

For Claude Code working in **HDDSynthPro**. Read this repo's CLAUDE.md first; everything below keeps its rules (core 1 never touches the UART or SD; core 0 never waits; pure, host-tested modules; SemVer and CHANGELOG with every change).

## Background

HDD Synth firmware **v0.20.0** added **modelled drives**: a sample pack can be a `drive.json` instead of WAVs, and the synth synthesises the drive live, with a virtual head that seeks with a real actuator's physics (a voice coil's bang-bang seek, or a stepper's steps). Try it in the browser: <https://maverickuk.github.io/HDDSynth_Native/drive-lab/>.

Today the synth only gets `$HDA` (busy, class, op) from HDDSynthPro, so its head goes to made-up positions: a plausible "file copy" or "random access" workload. HDDSynthPro knows each command's real LBA, so it can tell the synth where the head is really going.

HDD Synth firmware **v0.21.0** (HDDSynth_Native) has the receiving side of everything below, tested on the synth with the sentences typed in over USB.

There are two parts:

1. **`$HDE`, head targets** (HDDSynthPro → synth): where each command goes. Always on with the activity link.
2. **Authentic timing** (a new setting, off by default): HDDSynthPro holds each command for as long as the modelled drive would take (the seek, the wait for the sector to come round, the transfer), so the host really runs at the drive's pace, and what the synth plays matches what the host experiences. The model comes from the synth (`$HDM`, synth → HDDSynthPro, the link's other direction, unused until now) or from a preset in `HDDSYNTH.INI`.

## Part 1: `$HDE`, head targets (protocol version 2)

```
$HDE,2,<pos>,<op>,<sectors>*<checksum>\r\n
```

| Field | Value |
|---|---|
| `2` | Protocol version (`$HDA` stays version 1, unchanged) |
| `pos` | The command's first LBA as a fraction of the disk: `lba * 65535 / (total_sectors - 1)`, 0-65535 (64-bit maths). The synth needs no geometry: it places the fraction on the stroke itself (zoned recording puts LBA 0 at the outer edge) |
| `op` | `R` read, `W` write, `-` neither (as `$HDA`'s op) |
| `sectors` | The command's sector count |
| checksum | As `$HDA`: two upper-case hex digits, the XOR of every byte between `$` and `*` |

Known answers (check them in the host tests):

```
$HDE,2,0,R,8*21
$HDE,2,32768,W,128*2F
$HDE,2,65535,R,1*28
```

**When to send:** for each media command (`activity::isMediaCommand`) as it starts, from the same command events the classifier already gets. At most one `$HDE` every 2 ms: if commands come faster, keep only the latest unsent target and send that next. A real head can't follow faster, and the synth merges targets anyway (its head always heads for the newest).

**Sharing the UART with `$HDA`:** the link writes only into an empty 32-byte TX FIFO, and at 115200 baud an `$HDE` (up to 24 bytes) takes about 2 ms to go out. So:
- An `$HDA` that is due (a change, or the 200 ms heartbeat) goes first; `$HDE` takes the gaps.
- The `$HDA` heartbeat keeps the link alive as before. The synth also treats `$HDE` as a sign of life.
- Option for later, not this brief: a faster link (both ends' `LINK_BAUD`), tested on the rig first.

**What the synth does with it:** while `$HDE` keeps arriving (any within the last second), each target moves the head and the virtual workload stops. Another sector on the same track doesn't move the head; a few tracks away it ticks; further, it seeks. If they stop, the synth goes back to the virtual workload from `$HDA`. A synth older than v0.21.0 ignores `$HDE` (it rejects sentences it doesn't know).

## Part 2: authentic timing

### The setting

A new key in `HDDSYNTH.INI` (`src/drive_config.*`, and `docs/settings.md`'s table):

| Key | Values | Default |
|---|---|---|
| `seek_timing` | `off`; `synth` (match the modelled drive the HDD Synth is playing, from its `$HDM`); or a preset, `stepper85`, `voice91`, `voice97`, `fdb05` | `off` |

- `off`: as now, as fast as the card allows.
- `synth`: the model from the latest `$HDM`. Until one arrives, or while the synth is playing a recorded pack (`$HDM,2,-`), commands aren't held. If `$HDM` stops for 5 s (the synth sends one every 2 s), stop holding too.
- A preset: that model, whatever the synth plays. The four presets are the synth's: a 1985 stepper, a 1991 and a 1997 voice coil, and a 2005 7200 rpm drive.
- Console: `S` shows the setting and the model in use; `s` shows `seek_timing=` with the model's rpm and actuator, how many commands were held, and the average and longest hold. A console key to switch it off and on until restart, like `W`, `A` and `L`, would help benchmarking.

### `$HDM`: the model, from the synth

On the link's other direction (synth J6 pin 4, GP24, UART1 TX → HDDSynthPro GPIO 1, UART0 RX; J3 pin 4 on the PCB), the synth sends every 2 s, and at once when its pack changes:

```
$HDM,2,<V|S>,<rpm>,<cylinders>,<fullStroke ms x10>,<track ms x10>,<settleDamp x100>,<step ms x10>,<stepRamp x100>,<zoned 0|1>*hh
$HDM,2,-*hh          the synth is playing a recorded pack: no model
```

Example, the `voice97` preset:

```
$HDM,2,V,5400,6000,190,20,45,30,0,1*1B
```

- Parse it with HDD Synth's `parseModelSentence()` (below). Accept a later version number; ignore extra fields.
- **Receiving without waiting:** turn on UART0's receive side (GPIO 1's UART function). Read the RX FIFO on each core 0 loop pass and assemble lines (your `activity_link::Receiver` pattern, for `$HDM`). An `$HDM` is about 45 bytes, more than the 32-byte FIFO: either read the FIFO often enough (it fills in about 2.8 ms at 115200), or use the RX interrupt into a small buffer. A lost sentence doesn't matter: the next comes 2 s later.
- **Keep GPIO 1 safe:** pull it up when unconnected (a UART line idles high), so a floating pin can't make noise; reject bad lines and count them.
- `pytest -m link` / console `U` (the loopback) should now check both directions.

### The model: copy it from HDD Synth

Copy these pure C++ files from HDDSynth_Native (`src/`) into this repo, unchanged except for includes, and add them to the host tests:
- `drive_params.h/.cpp`: the presets (`drivePreset()`).
- `drive_timing.h/.cpp`: `seekTimeUs()`, `rotationalWaitUs()`, `transferUs()`, `revolutionUs()`, `lbaToStroke()`, and the `$HDM` format/parse. `seekTimeUs()` runs the synth's own head equations without the sound; HDD Synth's host tests hold it to the synth's engine (within 2 ms). It depends on `sentence.h/.cpp` for the checksum; copy that too, or adapt it to your own checksum helper.

Note in CLAUDE.md that they are copies, and that they change with the synth (the version comes from HDDSynth_Native's git history).

### Holding each command

For each media command, while `seek_timing` has a model:

```
x       = lbaToStroke(lba / total_sectors, model.zoned)              where the head must go
seek    = seekTimeUs(model, head_x, x)                                0 on the same track
arrive  = max(now, head_free_at) + seek                               the head can't start until it's free
wait    = rotationalWaitUs(model, arrive, lba, sectors_per_track)     the sector coming round
ready_k = arrive + wait + transferUs(model, k + 1, sectors_per_track) sector k (0-based) under the head
done    = ready_(n-1);   head_x = x;   head_free_at = done
```

- **The clock:** one free-running microsecond clock (`time_us_64()`) for everything. The platter's angle is that clock modulo one revolution, so a sequential read that goes straight on finds its next sector arriving and waits almost nothing, as a real drive does.
- **`sectors_per_track`:** the geometry's sectors per track (`identify`'s default is 63). It only sets the rotational and transfer times; the stroke position comes from the LBA fraction.
- **Where to hold, without waiting:**
  - Core 0 already fills the sector buffer and posts the result to core 1, which then sets DRQ (reads) or completes the command (writes).
  - With timing on, core 0 fills the buffer as now, then keeps the result pending until `ready_k`, checking each loop pass, so it never blocks.
  - The drive shows BSY meanwhile, which is legal and what a real drive does during a seek. Never hold IORDY for this.
- **Read-ahead and write-behind** stay as they are underneath: they keep the card from being the bottleneck. The timing only decides when each sector is *released* to the host. A sector fetched early is held until its `ready_k`.
- **Writes:** the same model (a real drive seeks and waits for the sector to write too). With write-behind, release each sector's acknowledgement at its `ready_k`.
- **DMA:** hold the DMA burst for the command (or each block of it) to the same `ready_k` times.
- **Commands that move no data** (verify, seek): `SEEK` moves the head (`seek` only); others as now.
- **Limits:** the slowest case is the 1985 stepper's full stroke, about 0.53 s, plus a turn. That's well inside host and adapter timeouts (seconds). Never hold a command longer than 1 s whatever the model says.
- **`$HDE` with timing on:** send each command's target when its seek starts (`max(now, head_free_at)`), not when it arrives, so the synth's head moves when HDDSynthPro's does. Commands then arrive at the pace of real seeks, so the 2 ms rate limit rarely bites.

## Tests

- **Host, `$HDE`:** the known answers above; `pos` at LBA 0, the middle and the last LBA; the 2 ms rate limit keeping only the latest; `$HDA` taking priority.
- **Host, `$HDM`:**
  - the example above parses to the `voice97` preset (`seekModelOf()` of `drivePreset("voice97")`);
  - `$HDM,2,-` means none;
  - a bad checksum or too few fields is rejected;
  - the 5 s timeout.
- **Host, the hold** (a pure scheduler, fed a fake clock):
  - same track: no seek;
  - a full-stroke seek takes `seekTimeUs(m, 0, 1)`;
  - a sequential run waits about one sector per sector;
  - random reads average seek + half a revolution;
  - `head_free_at` makes back-to-back commands queue;
  - the 1 s cap.
- **Rig:**
  - With `seek_timing = voice97`, 300 random 4 KB reads (your activity test's random phase) run at about 1 / (average seek + half a turn + transfer). Compute the expected rate with the same `drive_timing` functions over the same LBAs, and pass within ±20%.
  - Sequential 8 MB reads run at about the modelled media rate.
  - `seek_timing = off` benchmarks as before. The other rig tests run with it off.
- **With the synth** (EXPERIMENTAL_HDDSYNTHPRO_SUPPORT on, a modelled pack):
  - the synth's `s` shows `heads=` rising and `(arriving)`;
  - HDDSynthPro's `S` shows the model from `$HDM`, and it follows a pack change on the synth within 2 s (the BB48 rig needs its GPIO 1 jumper for this);
  - a file copy sounds like steady reading with track ticks, and random access like seeks.

## Wiring

No new wire: the link is a UART pair, and `$HDM` uses its other direction (the synth's TX into HDDSynthPro's RX), which version 1 left unused.

- **The PCB:** J3 already carries it. Pin 4 is LINK_RX (GPIO 1) through a resistor, in the same pin order as the synth's J6, so the straight 4-way cable connects both directions (`hardware/tools/check_netlist.py` checks it).
- **The BB48 rig,** wired with TX and GND only, needs the one jumper `docs/activity-link.md` lists as "reserved, not connected": GPIO 1 (EXT1 pin 5) through 1 kΩ to the synth's J6 pin 4 (GP24). Update that table to show it in use.

Only `seek_timing = synth` uses this direction; `$HDE` and the presets work without it.

## Docs and version

- `docs/activity-link.md`: protocol version 2 (`$HDE`, `$HDM`), the rig's GPIO 1 jumper, and the status.
- `docs/settings.md`: `seek_timing`.
- `welcome/README.TXT`'s settings, if it lists them.
- CLAUDE.md: the copied files.
- A MINOR version, a CHANGELOG entry, and the tag, as CLAUDE.md describes.

## Not in scope

- **Spin-up and spin-down from the drive's power commands:** STANDBY and IDLE would make the synth spin down and up. The synth's modelled drive has the sounds; that's a later protocol addition.
- **A faster link baud rate.**
- **Read and write sounds that differ:** `op` is sent; the synth doesn't use it yet.
