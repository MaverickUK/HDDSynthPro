# The SynthMinder protocol

**Status: version 1, implemented in 0.36.0-dev.1 (host-tested; not yet run with a PC).** This is the reference copy: [SynthMinder](https://github.com/MaverickUK/SynthMinder)'s `docs/protocol.md` points here. SynthMinder's demo (`DEMO.PAS`) and unit tests follow it, and the HDD Synth's side is HDDSynth_Native 0.25.0. Change the three together.

```
SYNMIND.EXE ──IDE: command 8Dh──▶ IDE Synth Bay ──Synth Link (UART)──▶ HDD Synth
            ◀── reply sectors ───               ◀── $HDS $HDP $HDK ───
```

SynthMinder talks to the Bay through a vendor-specific ATA command. Each request is one **sentence** in the HDD Synth's format (`$BODY*CS`, CS the XOR of BODY's characters in hex), and so is each line of the reply:

- **`$HD…` sentences** go to the synth: the Bay sends them on over the Synth Link unchanged, and returns what the synth answers. They are exactly the HDD Synth companion app's (`$HDQ`, `$HDC` → `$HDS`, `$HDP`, `$HDK`; HDDSynth_Native's README), so the synth has one protocol for both.
- **`$SB…` sentences** (Synth Bay) are for the Bay itself.

## The ATA command

| Register | SEND | RECEIVE |
|---|---|---|
| Features | `01h` | `02h` |
| Sector Count | `1` | `1` |
| LBA Low | `53h` (`S`) | `53h` |
| LBA Mid | `4Dh` (`M`) | `4Dh` |
| LBA High | protocol version, `1` | `1` |
| Device | either of the Bay's devices (`A0h` or `B0h`) | the same |
| Command | `8Dh` | `8Dh` |
| Protocol | PIO data-out, one sector | PIO data-in, one sector |

- `8Dh` is in ATA's vendor-specific range (80h-8Fh). The `S` `M` in LBA Low and Mid mark it as SynthMinder's: the Bay aborts (`ABRT`) an `8Dh` without them, any other Features value or version, so a stray `8Dh` from some other program does nothing. Firmware without SynthMinder support aborts it as an unknown command, which is how SynthMinder knows to say so.
- **SEND**: the Bay shows DRQ, SynthMinder writes the request sector, and the Bay shows BSY while it checks it and starts it (sends it to the synth, or runs it).
- **RECEIVE**: the Bay shows **BSY until the reply is ready**, then DRQ, and SynthMinder reads one sector. A reply that doesn't fit in one sector has the *more* flag set: SynthMinder sends RECEIVE again for the rest, up to 16 sectors. The Bay never holds IORDY for this: like a read, the wait is BSY.
- **How long:** the Bay answers within 1 s, whatever happens. A relayed request that the synth doesn't answer in 500 ms gets `$SBK,1,RELAY,ERR,nosynth`.
- RECEIVE with no request waiting answers `$SBK,1,REQUEST,ERR,none`.
- **Interrupts:** SynthMinder sets nIEN while it runs the command and clears it after, so the BIOS's IRQ 14 handler sees nothing it didn't ask for. The Bay raises INTRQ as for any command when nIEN is clear.
- **Disk caches:** SynthMinder asks SMARTDRV to write its cache out before each exchange (INT 2Fh, AX=4A10h, BX=1), so the cache can't start a write between its register writes.

### The sector

Both ways, 512 bytes:

| Offset | Size | |
|---|---|---|
| 0 | 4 | `SMND` |
| 4 | 1 | protocol version: `1` |
| 5 | 1 | flags: bit 0 *more* (a reply continues in the next RECEIVE) |
| 6 | 2 | sequence number, little-endian: SynthMinder counts its requests; a reply carries its request's |
| 8 | 2 | text length in bytes, little-endian, up to 496 |
| 10 | 6 | zero |
| 16 | 496 | text: sentences, each ending in a line feed (`0Ah`; a CR before it is ignored). A sentence is never split between sectors. |

A request holds one sentence (version 1). A reply holds as many as the answer needs.

## Sentences for the synth (relayed)

As the HDD Synth's companion protocol, version 1, unchanged:

| Request | Reply |
|---|---|
| `$HDQ,1,STATUS` | `$HDS,1,<firmware>,<state>,<packIndex>,<packCount>,<volume>,<balance>,<supercapMv>` |
| `$HDQ,1,PACKS` | `$HDP,1,<index>,<count>,<folder>,<name>,<kind>` for each pack (kind `M` modelled, `R` recorded; left out before synth v0.24.0) |
| `$HDQ,1,MODEL` | `$HDM,2,...` (the modelled drive) |
| `$HDC,1,VOL,<0-100>` | `$HDK,1,VOL,OK,` or `ERR,<reason>` |
| `$HDC,1,BAL,<0-100>` | `$HDK,1,BAL,...` |
| `$HDC,1,PACK,<index>` | `$HDK,1,PACK,...` (reasons `busy`, `demo`, `range`) |

The Bay knows how many lines each answer has (`STATUS` one, `PACKS` as many as `<count>`, a command one `$HDK`), so it doesn't wait out a timeout after the last one.

## Sentences for the Bay

### `$SBQ,1,STATUS`: how the Bay is set up

One `$SBV,1,<key>,<value>` per value. SynthMinder shows the keys it knows by name and any others as they come, so later firmware can add keys without a new version.

| Key | Value |
|---|---|
| `firmware` | e.g. `0.35.0` |
| `build` | `bb48_drive` or `pcb_drive` |
| `device0` | the hard disk image served, e.g. `ICL386MB.IMG`; `cdrom` with `device0 = cdrom`; `generated` or `welcome` |
| `device0_next` | the image `HDDSYNTH.INI` names, when it differs from `device0` (it's served from the next start) |
| `device1` | `none`, `cdrom`, or the image or folder |
| `disc` | the CD-ROM's disc, `-` for none |
| `bus` | the bus program and mode, in words, e.g. `PIO 3 data lane` |
| `seek_timing` | e.g. `off`, `synth 5400 rpm` |
| `card` | e.g. `SDIO high speed` |
| `link` | the Synth Link: `up`, `lost` or `off` |

### `$SBQ,1,FILES`: the images on the card

One `$SBF,1,<index>,<count>,<name>,<sizeKB>,<kind>` per file in the card's root that the Bay could serve: kind `H` for a hard disk image (`.IMG`), `C` for a CD-ROM image (`.ISO`, `.CUE`). Names are 8.3, in alphabetical order.

### `$SBC,1,DISC,<name>`: change the CD-ROM's disc

As console `M`: the drive reports the disc changing (removal, new media, unit attention), as if the disc had been swapped. `-` ejects. Reply `$SBK,1,DISC,OK,` or `ERR,` with `nocdrom` (no CD-ROM drive set up), `notfound`, `busy`.

### `$SBQ,1,SETTINGS`: the settings SET can change (since 0.36.0)

One `$SBS,1,<key>,<next>,<now>` per setting, in this order: `<next>` is what `HDDSYNTH.INI` says (read afresh, so it shows a change saved with SET), used from the next start; `<now>` is what the drive runs with. They differ after a SET, or after the console changed one until a restart. Firmware without it answers `$SBK,1,SETTINGS,ERR,unknown`.

| Key | Values |
|---|---|
| `device1` | `none`, `cdrom` (a CD-ROM drive, with `cdrom`'s disc), or an image (`.IMG`) or disc (`.ISO`, `.CUE`) on the card |
| `cdrom` | the CD-ROM's disc at the start: an `.ISO` or `.CUE` on the card |
| `seek_timing` | `off`, `synth`, or a preset: `stepper85`, `voice91`, `voice97`, `fdb05` |
| `activity_link` | `on`, `off` |
| `write_behind` | `on`, `off` |
| `read_ahead` | `on`, `off` |
| `sd_high_speed` | `on`, `off` |

### `$SBC,1,SET,<key>,<value>`: change a setting in `HDDSYNTH.INI`

As console `E`: the file is rewritten with that one setting changed, and it takes effect at the next start, never at once (the PC is running from the disk). `device0`, to an image on the card, and since 0.36.0 every key SETTINGS lists, with the values above. Reply `$SBK,1,SET,OK,` or `ERR,` with `key` (not one SET may change), `value` (not a value it takes), `notfound` (no such image or disc on the card) or `card` (the card or `HDDSYNTH.INI` couldn't be read or written).

### Errors

A request that isn't a valid sentence: `$SBK,1,REQUEST,ERR,invalid`. One the Bay doesn't know: `$SBK,1,<its first field>,ERR,unknown`. Values never contain `,`, `*` or `$`: the Bay replaces them with `_`, as the synth does in pack names.

## In this firmware

- **8Dh** is `ata::Device::synthMinder()` (`ata_device.cpp`): the checks above, then a one-sector write (SEND) or read (RECEIVE) through `beginWrite`/`beginRead`, with the `Fetch` marked `message`. On the hard disk only: a CD-ROM alone (`device0 = cdrom`) aborts it, as firmware without SynthMinder does. With a CD-ROM as device 1, SynthMinder uses device 0.
- **Core 0** (`bay_service.cpp`) passes those fetches to `synthminder::Service` (`synthminder.*`, host-tested in `test/native/test_synthminder.cpp`). A RECEIVE whose answer isn't in yet is held, BSY, and answered from `loop()`; if another command has started meanwhile, the late answer is dropped and its buffer left alone.
- **Relays** go out on the link's TX (GPIO 0) when its FIFO is empty, before the next `$HDA` or `$HDE`; the synth's lines come back on GPIO 1, read by an interrupt into a 1 KB ring. Relaying needs `activity_link = on` here and `EXPERIMENTAL_HDDSYNTHPRO_SUPPORT` on the synth, and both wires ([activity-link.md](activity-link.md)).
- **Limits:** a request's sentence up to 128 characters, a relayed one up to 30 (the TX FIFO; SynthMinder's are under 20); a reply up to 3 KB of text (about 34 packs in a pack list: past that the list is cut short); FILES lists up to 32 images.
- **Where it differs from the draft:**
  - FILES lists the card's root afresh each time (a few milliseconds), rather than once and cached, so it's never out of date.
  - A `$HD...` request the drive doesn't know the answer to (anything but `$HDQ` `STATUS`, `PACKS`, `MODEL` and `$HDC`) isn't relayed: `$SBK,1,<its first field>,ERR,unknown`.
  - DISC's `notfound` also covers a name that isn't an `.ISO` or `.CUE`; `-` and `none` eject, `test` is the test disc. The reply comes once the change has started (it completes a moment later, as console `M`'s does).
  - SET's value must be an `.IMG` on the card; a further reason, `card`, says HDDSYNTH.INI couldn't be read or written.
- **On the console:** `s` shows `sm_requests`, `sm_relays`, `sm_timeouts` and `link_rx_lost`; a change SynthMinder makes prints a `SYNTHMINDER` line.

## The HDD Synth

HDDSynth_Native 0.25.0 answers `$HDQ` and `$HDC` arriving on the link (J6 pin 3) as it does on USB, with the replies on the link's TX (J6 pin 4, the wire that carries `$HDM`).

## FDD Synth Duo

Not yet. The floppy bus has no spare command, so it needs a way of its own; see SynthMinder's roadmap.
