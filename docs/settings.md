# Settings: HDDSYNTH.INI

The drive reads its settings from `HDDSYNTH.INI` in the root folder of the microSD card when it starts. It's plain text: edit it on a computer with any text editor, or from the drive's serial console. Without the file, or for any setting left out, the drive uses the defaults, so a card without it behaves as before.

```ini
# HDDSynthPro settings. Changes take effect when the drive restarts.
image = HDDSYNTH.IMG
mode = auto
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

**Which disk the drive serves**, in order:
1. `HDDBENCH.IMG`, if console `J` asked for it (a scratch disk for tests; a power cycle undoes it).
2. The `image` from the settings, or with `image = none` the built-in test disk.
3. If that isn't on the card, or is empty: `HDDSYNTH.IMG`.
4. If there's no card, or neither image can be used: the built-in 64 MB test disk, whose writes stay in RAM until the drive restarts.

An image is used as it is: its size (a multiple of 512 bytes) is the disk's size. Console `I` creates a blank 64 MB `HDDSYNTH.IMG`.

**Transfer modes:** this firmware supports PIO mode 0 only. Any other mode is accepted in the file, with a warning, and the drive uses `auto` until that mode is added, so a settings file written now keeps working later.

## Pulling the card out

Eject the disk on the computer first, as you would before unplugging any disk. If the card comes out while the drive is serving an image, the computer gets read and write errors for that disk until the card is back. The drive serves the image again (`CARD back` on the console) only if **the same card** comes back with **the image unchanged**. A different card, or the image edited on another computer meanwhile, isn't served (`CARD changed`): the computer still has the old disk's file system in its cache, and its next writes would corrupt the new one. To use a different card or an edited image, eject the disk on the computer, then restart the drive (console `!`, or power).

## From the serial console

| Key | Does |
|---|---|
| `S` | Reports the card, then `CONFIG` lines: what the file says, which image is being served, any image that couldn't be used and why, and any warnings |
| `C` | Creates `HDDSYNTH.INI` with the defaults and comments, if there isn't one |
| `E` | Sets one setting: type `E`, then e.g. `image = GAMES.IMG`, then Enter. The rest of the file, comments included, stays as it was |
| `!` | Restarts the drive, so the settings take effect (keeps a `J` choice) |

`s` also shows `image_name=` (the image being served) and `config=` (`none`, `present` or `truncated`, as read at start-up).

The drive never formats the card, and the only files it writes are the image it serves, images created with `I` or `K`, and `HDDSYNTH.INI` (only with `C` or `E`).
