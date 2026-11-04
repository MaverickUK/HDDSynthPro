# Brief: the HDD Synth's screen for the CD-ROM drive, and swapping discs from it

> **Design, 4 Nov 2026; nothing built yet.** Protocol version 3 of the activity link ("Synth Link"), for the HDD Synth hardware revision that gives the link its own pins. Build the HDDSynthPro side only after the CD-ROM drive has run on a real PC through the bench ([bench-checklist.md](bench-checklist.md)).

For whoever builds either side: Claude Code in **HDDSynth_Native** (the synth) or in **HDDSynthPro** (the drive). Each repo's CLAUDE.md comes first. On the HDDSynthPro side: core 1 never touches the UART or SD; core 0 never waits; pure, host-tested modules; SemVer and a CHANGELOG entry with every change.

## Background

[FDDSynthPro](https://github.com/MaverickUK/FDDSynthPro) shows its floppy drive on an SSD1306 OLED: a home screen (the image's name, track, head, reading or not), a head-trace screen, and a sounds screen, stepped through with an action button. The rotary encoder sets the volume, pressing it picks a disk image from the SD card and inserts it, and holding the action button ejects. It remembers the disk across power-off, as a GoTek does.

The HDD Synth already has an OLED, and HDDSynthPro now has a CD-ROM drive: alone (`bb48_cdrom`), or as device 1 beside a hard disk (`bb48_hdcd`, `device1 = cdrom`), with discs changed at run time from its console (`M`, 0.30.0-dev.7). This brief gives the HDD Synth an FDDSynthPro-style screen for the CD-ROM, and lets it swap discs. **The synth draws the screen and owns the controls; HDDSynthPro reports and obeys.**

Two things have to change first:
- **Pins.** On the HDD Synth Mk5, header J6 carries either the OLED or the link (UART1 on the OLED's I²C pins, GP24/GP25), so `EXPERIMENTAL_HDDSYNTHPRO_SUPPORT` switches the OLED off. **The next hardware revision gives the link dedicated pins** (TX, RX and GND, and 5 V for powering HDDSynthPro's PCB through its J3), so the OLED and the link run together. Everything below assumes that revision.
- **Line length.** Both sides' receivers drop lines over 80 bytes (`SENTENCE_MAX_LINE` in the shared `sentence.h`, `activity_link::MAX_LINE`). Disc names can be longer, so they're sent shortened for display (below), and a disc is asked for by its place in the list plus a checksum of its full name. `sentence.h` stays unchanged.

## Is the link fast enough?

Yes, by an order of magnitude. At 115,200 baud, 8N1, the link carries about 11,500 bytes a second, and a line of 30 bytes takes 2.6 ms.

| Traffic | When | Size | Load |
|---|---|---|---|
| `$HDA` (today) | on change, and every 200 ms | 20 bytes | about 100-400 bytes/s |
| `$HDE` (today, the hard disk's head) | each media command, at most every 2 ms | about 20 bytes | up to 10 KB/s in a burst of tiny commands; about 1-2 KB/s in real use |
| `$HDU`, each drive's activity | on change, and every 200 ms with `$HDA` | 17 bytes each | about 200 bytes/s |
| `$HDP`, the CD-ROM's position | each READ packet, at most every 10 ms | about 21 bytes | up to 2 KB/s while reading |
| `$HDD`, the disc | on change, and every 2 s | up to 80 bytes | negligible |
| `$HDL`, the disc list | only when the synth asks: a page of up to 16 | up to 80 bytes each | a page in about 0.1 s |

`$HDE`'s worst case is already rate-limited and newest-wins, and so is `$HDP`. If the link ever ran short, both RP2350s run their UARTs at 921,600 baud on these short wires; nothing here needs it.

## Protocol version 3

The same framing as versions 1 and 2: ASCII lines `$NAME,fields*hh\r\n`, the checksum two upper-case hex digits, the XOR of every byte between `$` and `*`. A receiver ignores unknown sentences, lines with a bad checksum or over 80 bytes, and extra fields after the ones it knows. `$HDA`, `$HDE` and `$HDM` don't change: a synth that knows only version 2 keeps working and ignores the rest.

**Names** in `$HDD` and `$HDL` are the last field, so they may contain commas (a FAT name can; it can't contain `*`). Printable ASCII only: any other byte becomes `?`. A name longer than 48 bytes is sent as its first 47 and `~`. The synth may sort and further shorten for its font.

**Units:** `0` device 0 or `1` device 1, as HDDSynthPro's trace has them. The CD-ROM is unit 0 in `bb48_cdrom` and unit 1 in `bb48_hdcd`; `kind` says which drive is which. There's at most one CD-ROM.

### HDDSynthPro → synth

**`$HDU`: each drive's activity.** Sent for each unit on any change, and with every `$HDA` heartbeat.
```
$HDU,3,<unit>,<kind>,<busy>,<op>*hh
```
| Field | Value |
|---|---|
| `kind` | `D` hard disk, `C` CD-ROM |
| `busy` | `0` or `1`, by the activity LED's rule (a command running, or one started in the last 25 ms) |
| `op` | `R` read, `W` write, `S` seek or verify, `-` none (the last media command) |

**`$HDD`: the disc.** On any change, every 2 s, and when asked (`$HDQ,3,STATE`).
```
$HDD,3,<unit>,<tray>,<frames>,<index>,<name>*hh
```
| Field | Value |
|---|---|
| `tray` | `I` a disc in, `E` empty, `C` changing (between eject and insert) |
| `frames` | The disc's 2,048-byte frames, 0 when empty |
| `index` | Its place in the disc list (`$HDL`), or `-` (the test disc, or an ISO not in the list) |
| `name` | The ISO's name, `test` for the test disc, `-` when empty |

**`$HDP`: where the CD-ROM is reading.** For each READ(10) or READ(12) packet, as it starts; at most one every 10 ms, newest wins (as `$HDE`).
```
$HDP,3,<unit>,<frame>,<count>,<op>*hh
```
`frame` is the packet's first frame (absolute; with `$HDD`'s `frames` the synth draws a bar, or minutes and seconds at 75 frames a second, as a CD player would), `count` its length in frames, `op` `R` (`S` for SEEK and VERIFY).

**`$HDL`: the disc list,** in reply to `$HDQ,3,LIST`. One line per disc, then an end line.
```
$HDL,3,<total>,<index>,<crc>,<name>*hh
$HDL,3,<total>,END*hh
```
| Field | Value |
|---|---|
| `total` | How many discs the list holds |
| `index` | This disc's place, from 0 |
| `crc` | CRC-16/CCITT-FALSE of the full file name's bytes (poly 1021h, initial FFFFh, no reflection, no final XOR; `123456789` gives `29B1`), four upper-case hex digits |
| `name` | Shortened as above |

**The list:** the `*.ISO` files (any case) in the card's root folder, in directory order, which is stable while the card isn't changed. HDDSynthPro keeps no copy (RAM is short: `bb48_hdcd` uses 93%); it walks the directory for each page, a step per loop pass. The synth sorts what it shows if it wants to.

**`$HDR`: the answer to a request** that changes something.
```
$HDR,3,<verb>,<OK|ERR>,<detail>*hh
```
`OK` with the disc's name, or `ERR` with a short reason: `list changed`, `not found`, `no cd-rom`, `busy` (a change already under way), `no card`.

### Synth → HDDSynthPro

On the reply wire that already carries `$HDM`.

| Sentence | Meaning |
|---|---|
| `$HDQ,3,STATE*hh` | Send `$HDU` and `$HDD` now (the synth just started, or the link came back) |
| `$HDQ,3,LIST,<first>,<count>*hh` | A page of the disc list: `count` up to 16 |
| `$HDQ,3,LOAD,<index>,<crc>*hh` | Put in disc `index`; HDDSynthPro walks to it, checks the name's CRC, and answers `$HDR` (`ERR,list changed` if it differs) |
| `$HDQ,3,EJECT*hh` | Take the disc out |

A load is the console's `M` underneath (0.30.0-dev.7): HDDSynthPro ejects (`$HDD ... C`), waits for core 1 to take it once the drive is idle, swaps the file, inserts (`$HDD ... I`), and the host gets a media-removal event, then new media and a unit attention (28h), as from a real tray.

### Known answers

Check both sides' host tests against these (the CRCs are of the full names `DOOM_SE.ISO` and `HDDSYNTH.ISO`):

```
$HDU,3,1,C,1,R*57
$HDU,3,0,D,0,-*2F
$HDU,3,1,C,1,S*56
$HDD,3,1,I,11431,3,DOOM_SE.ISO*3D
$HDD,3,1,E,0,-,-*3F
$HDD,3,1,C,0,-,-*39
$HDD,3,0,I,32768,-,test*01
$HDP,3,1,4096,32,R*2A
$HDP,3,1,0,1,R*21
$HDL,3,12,3,9AD5,DOOM_SE.ISO*5D
$HDL,3,12,0,4559,HDDSYNTH.ISO*0A
$HDL,3,12,END*13
$HDR,3,LOAD,OK,DOOM_SE.ISO*54
$HDR,3,LOAD,ERR,list changed*6E
$HDR,3,EJECT,OK,-*19
$HDQ,3,LIST,0,16*5B
$HDQ,3,LOAD,3,9AD5*52
$HDQ,3,EJECT*33
$HDQ,3,STATE*39
```

## The synth's screens (HDDSynth_Native)

Modelled on FDDSynthPro's, drawn on the existing OLED from what the link reports. The action button steps through them; the encoder keeps the volume.

1. **CD-ROM home:** the disc's name (scrolling if long), a tray icon (in, empty, changing), the position as a bar or `MM:SS` from `$HDP` and `$HDD`'s frames, and the activity (reading or seeking, from `$HDU`). "No disc" when empty; "No link" when the link is lost (no valid sentence for 1 s, as today).
2. **Disc picker:** pressing the encoder on the home screen sends `$HDQ,3,LIST` page by page and shows the names (sorted, if the synth buffers them: 64 names of 48 bytes is 3 KB); turning scrolls, pressing loads (`$HDQ,3,LOAD`), and the screen shows "Loading..." until `$HDR`, then returns home. Holding the action button ejects (`$HDQ,3,EJECT`).
3. **Hard disk** (with `bb48_hdcd`, or the hard disk build): the existing activity, and the head trace from `$HDE`, as FDDSynthPro's head-trace screen.
4. **Sounds:** as today.

Sounds for the CD-ROM (a spin-up when a disc goes in, a seek, the tray) are a natural next step, from the same sentences; not part of this brief.

## HDDSynthPro's side

All on core 0, in the CD-ROM builds (`bb48_cdrom`, `bb48_hdcd`); the hard disk build sends `$HDU` only.

1. **A send queue.** Today a sentence is written only when the UART's 32-byte FIFO is empty, so it must fit in 32 bytes; `$HDD` and `$HDL` don't. A ring of about 512 bytes, drained into the FIFO on every loop pass (never waiting), with the priorities: `$HDA` and `$HDU` first, then `$HDR` and `$HDD`, then `$HDP` and `$HDE` (newest wins), then `$HDL` pages. Pure C++ and host-tested, as `activity_link`'s sender is now.
2. **Per-drive activity:** `$HDU` from each unit's commands (the trace's CMD and DONE events already carry the unit), and `$HDP` from READ packets' first frame and count (also in the trace: `CMD ... a0 READ_10 <frame> <count>`).
3. **The disc list:** a walk of the root folder for `*.ISO` names, one entry per loop pass and only while no fetch is waiting, so serving the drive comes first. CRC-16 of each full name; names shortened and sanitised as above (pure, host-tested).
4. **Requests:** `$HDQ` parsed from the reply wire alongside `$HDM` (bad lines counted, never acted on), `LOAD` by the same path as console `M`, then `$HDR`.
5. **Console and stats:** `s` gains `hdq_valid`, `hdq_rejected`, `hdl_pages`, `disc_loads`; the loopback test (`U`) checks a `$HDQ,3,STATE` round trip.
6. **Tests:** formats and parsers against the known answers above, shared with a Python reference (`host/hddproto/activity_link.py`); the list walk and LOAD's CRC check on the host with a fake directory; on the rig, the loopback, then with the synth.

## Decisions for the owner

- **Remember the disc across power-off?** FDDSynthPro does. HDDSynthPro would have to write `cdrom = NAME` into `HDDSYNTH.INI` (the same code as console `E`), and today the firmware edits that file only from the console, by the owner's rule. Options: allow it for a disc chosen on the synth; keep the choice in RAM only (a power cycle goes back to `HDDSYNTH.INI`'s disc); or let the synth remember it and send `LOAD` after it starts.
- **Which folder holds the discs:** the root folder (as above, like every other image), or a `CD` folder, which would keep a long list out of the root.
- **Multi-disc games:** a later step could group `GAME_1.ISO`, `GAME_2.ISO`... so "next disc" is one press.
