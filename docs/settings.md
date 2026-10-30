# Settings: HDDSYNTH.INI

The drive reads its settings from `HDDSYNTH.INI` in the root folder of the microSD card when it starts. It's plain text: edit it on a computer with any text editor, or from the drive's serial console. Without the file, or for any setting left out, the drive uses the defaults, so a card without it behaves as before.

```ini
# HDDSynthPro settings. Changes take effect when the drive restarts.
image = HDDSYNTH.IMG
mode = auto
dma = off
write_behind = on
read_ahead = on
activity_link = on
seek_timing = off
isa = off
```

- One setting per line, as `key = value`. Spaces around `=` and the case of the key don't matter.
- `#` starts a comment (at the start of a line, or after a space), and so does `;` at the start of a line. A `#` inside a name, as in `DISK#1.IMG`, is part of the name.
- Windows line endings and Notepad's byte-order mark are fine. `[section]` lines are ignored.
- A mistake never stops the drive: an unknown key, a bad value or a setting given twice is reported (console `S`) and skipped.

## The settings

| Key | Values | Default |
|---|---|---|
| `image` | The disk image to serve: a file in the card's root folder, e.g. `GAMES.IMG` (quotes optional), or `none` for the built-in 64 MB test disk (its writes are lost when the drive restarts; the soak test uses it, since only its data can be checked on the cable) | `HDDSYNTH.IMG` |
| `mode` | The transfer mode to offer the host: `auto` (the fastest both sides support), or one of `pio0`–`pio4`, `mdma0`–`mdma2`, `udma0`–`udma6` to allow at most that one | `auto` |
| `dma` | `on` or `off`: whether `auto` may offer multiword DMA, where DMARQ is wired. Much faster with DMA-capable hosts (reads about 4 times, writes 5 times through the USB adapter) | `off` until DMA has had a long test |
| `write_behind` | `on` or `off`: the drive acknowledges each written sector while storing it on the card, so the host sends the next one meanwhile. About 50% faster writes to the card; if power is cut in the millisecond a sector is being stored, that sector is lost | `on` |
| `read_ahead` | `on` or `off`: the drive fetches the next sector while the host reads this one | `on` |
| `activity_link` | `on` or `off`: tell an HDD Synth what the drive is doing ([activity-link.md](activity-link.md)) | `on` |
| `seek_timing` | Hold each command for as long as a modelled drive would take: the seek, the wait for the sector to come round, the transfer. So the computer runs at that drive's pace, and an HDD Synth playing the same drive sounds as the computer feels. `off`: as fast as the card allows. `synth`: the drive the HDD Synth is playing (from its `$HDM`, which needs the link's second wire, GPIO 1; until one arrives, while it plays a recorded pack, or 5 s after it stops, nothing is held). A preset, whatever the synth plays: `stepper85` (a 1985 stepper), `voice91` and `voice97` (1991 and 1997 voice coils), `fdb05` (a 2005 7,200 rpm drive). No command is held over a second | `off` |
| `isa` | `on` for a 16-bit ISA IDE card (286, 386 and many 486 machines). These cards select the drive from the address alone, so memory and DRAM refresh cycles can select it too, and they pass IORDY to the whole ISA bus. The drive pulls IORDY low as soon as it's selected (as ATA's timing needs), so such a cycle would stall for good: that froze an ICL 386. With `on`, the drive lets go of IORDY once it has held it for about 23 µs, which a real access never needs, and such a cycle ends. Leave it `off` for USB adapters and later controllers, which never need it. IOCS16#, which these cards also need, is always on unless DMA is offered (GPIO 2, [wiring.md](wiring.md)) | `off` |

The on/off settings also take `yes`/`no`, `true`/`false` and `1`/`0`. The console's `W`, `A` and `L` switch write-behind, read-ahead and the activity link until the next restart, without changing the file, and `H` switches seek timing off and on (to compare speeds).

**Which disk the drive serves**, in order:
1. The built-in test disk if console `T` asked for it, or `HDDBENCH.IMG` if console `J` did (both for tests; a power cycle undoes them).
2. The `image` from the settings, or with `image = none` the built-in test disk.
3. If that isn't on the card, or is empty: `HDDSYNTH.IMG`.
4. If there's no card, or neither image can be used: the welcome disk.

**The welcome disk** is 64 MB, partitioned as a PC's disk would be, with one FAT16 volume, `HDDSYNTHPRO`, that macOS, Windows, Linux and DOS 3.31 or later can read. It holds `README.TXT`, which explains how to set the drive up, and `HDDSYNTH.INI` with the defaults, ready to copy to a card. Writes to it stay in RAM (up to 256 KB) until the drive restarts. To change the README, edit [welcome/README.TXT](../welcome/README.TXT): plain ASCII, at most 80 characters a line, `{VERSION}` for the firmware version. The build checks it and embeds it (`scripts/embed_welcome.py`). To see the disk before flashing, run `test/run_native_tests.sh`, then `.pio/native-tests/make_welcome_img welcome.img` and open `welcome.img`.

**The test disk** is 64 MB with every sector's content computed from its LBA, so the rig tests and the bus observer can check every byte read. It has no file system: a computer offers to initialise it.

An image is used as it is: its size (a multiple of 512 bytes) is the disk's size. Console `I` creates a blank 64 MB `HDDSYNTH.IMG`.

**Transfer modes:** this firmware supports PIO modes 0 to 3, and `auto` offers mode 3. `mdma0` to `mdma2` offer multiword DMA up to that mode as well (and PIO mode 3). DMA needs DMARQ and DMACK# wired: the PCB has them, the BB48 rig only if added by hand ([dma.md](dma.md)). The drive checks DMARQ when it starts, and without it offers PIO only, with a warning in `S`. With `dma = on`, `auto` offers DMA too, where DMARQ is wired. Any other mode (PIO 4, Ultra DMA) is accepted in the file, with a warning, and the drive uses `auto` until that mode is added, so a settings file written now keeps working later.

## Pulling the card out

Eject the disk on the computer first, as you would before unplugging any disk. If the card comes out while the drive is serving an image, the computer gets read and write errors for that disk until the card is back. The drive serves the image again (`CARD back` on the console) only if **the same card** comes back with **the image unchanged**. A different card, or the image edited on another computer meanwhile, isn't served (`CARD changed`): the computer still has the old disk's file system in its cache, and its next writes would corrupt the new one. To use a different card or an edited image, eject the disk on the computer, then restart the drive (console `!`, or power).

## From the serial console

| Key | Does |
|---|---|
| `S` | Reports the card, then `CONFIG` lines: what the file says, which image is being served, any image that couldn't be used and why, and any warnings |
| `C` | Creates `HDDSYNTH.INI` with the defaults and comments, if there isn't one |
| `E` | Sets one setting: type `E`, then e.g. `image = GAMES.IMG` or `write_behind = off`, then Enter. The rest of the file, comments included, stays as it was; a setting the file doesn't have yet is added at the end |
| `!` | Restarts the drive, so the settings take effect (keeps a `J` or `T` choice) |
| `T` | Restarts the drive serving the built-in test disk, card or no card, or back (a power cycle undoes it too); replug the adapter afterwards |

`S` shows the settings in use (`CONFIG dmarq=wired dma=off write_behind=on read_ahead=on activity_link=on isa=off`, with `dmarq` saying whether DMA can be offered at all), and `CONFIG seek_timing=voice97 model=voice 5400rpm 6000cyl stroke=19.0ms track=2.0ms zoned=1` (for `synth`, also when its last `$HDM` came). `s` shows `seek_timing=`, `seek_active=` (holding commands now), `seek_rpm=`, `seek_actuator=`, and `seek_commands=`, `seek_held=`, `seek_avg_us=`, `seek_max_us=`, `seek_total_us=`, `seek_capped=` (commands timed, how many waited, their modelled time, and any that hit the 1 s limit). `s` also shows `image_name=` (the image being served) and `config=` (`none`, `present` or `truncated`, as read at start-up).

The drive never formats the card, and the only files it writes are the image it serves, images created with `I` or `K`, and `HDDSYNTH.INI` (only with `C` or `E`).
