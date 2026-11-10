# One drive build per board (plan, 9 Nov 2026)

A brief for doing this work on the Windows rig. Read `CLAUDE.md` first: its rules hold throughout, especially *Rules that keep the bus safe*, *Core split* and the *Rig* notes for Windows.

## Progress (9 Nov, evening, Windows PC without the board)

- **Step 2 is done** on branch `one-build` (0.32.0-dev.1, `CHANGELOG.md`): `hdcd::Device` takes the CD-ROM on unit 0 or 1 (`cdSelected()` became one mask and one compare, `(device_reg_ & cd_mask_) == cd_match_`, for all three layouts: no cheaper or dearer than `bb48_hdcd`'s `cd_on_ && (device_reg_ & DEV_DEV)`, but measure); `device0 = cdrom` in `drive_config`; `main.cpp`, `ide_bus.cpp` and `bus_observer.cpp` without `HDDPRO_CDROM`/`HDDPRO_HDCD`; `bb48_cdrom` and `bb48_hdcd` gone from `platformio.ini` and CI; `conftest.py` finds a CD-ROM by `S`; the docs. Native tests (5,676 checks) and `pytest` (sim) pass; `check_core1_ram.py` passes on both drive ELFs (69 functions, as `bb48_hdcd`); RAM 94.1% on both.
- Found on the way and fixed: on the PCB, the card was parked (its PIO block lent to the diagnostics) *before* device 1's image or disc was opened, when device 0 served nothing from the card; and `M` into an empty drive opened the disc on a borrowed block. See the CHANGELOG.
- **Step 1, run later that evening on the PCB (CHANGELOG 0.32.0-dev.2):** `hdcd::Device` is *not* free: 0.31.0's `bb48_hdcd` with no CD-ROM wrote 20% slower than `bb48_drive` in PIO (0.90 MB/s against 1.13), dev.1 27% (0.83), and data reads moved from under 700 ns to 700-1,000. The owner chose **one binary, two loops** over the four-build fallback: dev.2 compiles core 1's code for `ata::Device` (the hard disk alone) and for `hdcd::Device` (with a CD-ROM), and `loop1` picks one at boot. A-B-A-B against 0.31.0: early answers 1.17/1.11 MB/s against 1.20/1.13, the lane 1.53/1.64 against 1.53/1.68, worst reads within 60 ns; the functions are byte-identical, about 3.6 KB further on in RAM. RAM 95.5%.
- **Step 3 so far (dev.3):** a CD-ROM as device 1 and alone as device 0 both pass `-m cdrom` through the adapter, with disc changes (`M`) on device 0. A CD-ROM as device 0 needed `ide_bus` (dev.3): on early answers the adapter never detected it. Still to do: slices 1-2, `pcb_drive` with SDIO (`S` says `bus=sdio`, a 30-minute `-m cardsoak`), the hour's soaks, and the release.
- **dev.5 (9-10 Nov, overnight):** the hard disk's core 1 code back at 0.31.0's addresses (the CD-ROM's in `CORE1_RAM_CD` after it): `pcb_drive` PIO reads equal 0.31.0's (1.20 MB/s), writes 1.11 against 1.13. The hour's soaks of `bb48_drive` and `pcb_drive` on dev.5 pass (test disk, early answers, full observer checks: 8.9 million sectors checked on the cable, 0 bad, `iordy_ignored=0`, worst read 2,110 ns, as 0.31.0's 2,070). **Still to do, with the owner:** a replug to check dev.5's CD-ROM (the card's settings serve `DOOM_SE.ISO` as device 1 again), slices 1-2 on a healthy adapter (it garbled its serial again overnight: restart the PC first), the card soak on SDIO (reseat the card: it's in SPI mode), then the release.
- **10 Nov, morning (PC restarted, adapter replugged, card reseated: on SDIO):** dev.5's CD-ROM works (Windows mounted `DOOM_SE` as D:; `-m cdrom` and slices 3-4 pass beside it, `mode = mdma2`). Overnight it only looked broken: the Evesham's `dma = off` and this adapter's DMA bit on every PACKET. Slices 1-2 then timed out waiting for a quiet console, as Windows polls a CD-ROM once a second; `wait_quiet()` now ignores those polls (host only). **Left:** a replug (the adapter went silent after a drive restart with the CD-ROM detected), then slices 1-2 with `device1 = none` (set, with the test disk via `T`), the 30-minute `-m cardsoak` on SDIO (`J`), put the owner's settings back, and the release.

## The goal

Go from five firmware builds to three:

| Today (`platformio.ini`) | After |
|---|---|
| `bb48`: listen only | `bb48`: unchanged |
| `bb48_drive`: hard disk(s), the card on SPI | `bb48_drive`: **hard disk, CD-ROM or both, by settings**, the card on SPI |
| `bb48_hdcd`: hard disk + CD-ROM as device 1 | (folded into `bb48_drive`) |
| `bb48_cdrom`: CD-ROM as device 0 | (folded into `bb48_drive`) |
| `pcb_drive`: hard disk(s), the card on SDIO | `pcb_drive`: **as `bb48_drive`**, the card on SDIO |

What the owner gets:
- one file to flash per board;
- the CD-ROM on the PCB with the card on SDIO (today the CD-ROM builds are SPI only);
- test results that carry over between configurations, since it's one binary. Core 1's speed depends on where its code sits in RAM, so a soak of one build says little about another.

Release it as **0.32.0**, from one short branch merged to `main` when done (the workflow since 9 Nov: one branch per piece of work, merged and deleted promptly; `main` is the only long-lived branch). Keep the environment names `bb48`, `bb48_drive` and `pcb_drive`, so `scripts/rig.sh`, `check_core1_ram.py`'s default ELF and the release file names change as little as possible.

## Where things stand

- The device is chosen at compile time in `src/drive_device.h`: `ata::Device` (hard disk), `atapi::Device` (`HDDPRO_CDROM`), or `hdcd::Device` (`HDDPRO_HDCD`: a hard disk as device 0 and, with `device1 = cdrom`, a CD-ROM as device 1; without a CD-ROM it serves what `ata::Device` does, plus one branch per access). `src/main.cpp` has about 37 `#if HDDPRO_CDROM` / `HDDPRO_HDCD` sections; `ide_bus.cpp` and `bus_observer.cpp` have one or two each.
- `hdcd::Device` (`src/hdcd_device.*`, host-tested in `test/native/test_hdcd.cpp`) runs on core 1 behind the same interface as `ata::Device`. Every per-access member is `always_inline` and checks `cdSelected()` (`cd_on_ && (device_reg_ & ata::DEV_DEV)`).
- With a CD-ROM as device 1, `bb48_hdcd` forces `ide_bus`, not early answers (0.30.0-dev.10: the USB adapter's register check failed on early answers; cause open). The CD-ROM's data moves by multiword DMA when offered; the Windows rig's adapter sets the DMA bit on every PACKET, so with a CD-ROM it needs `mode = mdma2`.
- **The measurements so far:**
  - `bb48_hdcd` with a CD-ROM, on the adapter, MDMA 2: the disk read 9.02 MB/s and wrote 13.25 (`bb48_drive`: 8.84 and 12.48), and a 60-minute soak was clean (0.30.0-dev.10, 4 Nov).
  - On the Evesham on the PCB (9 Nov), `bb48_hdcd` ran Doom from the CD beside the hard disk with every command good. Its data reads were never held past 1,100 ns, and every one was waited for.
  - **Not yet measured: `bb48_hdcd` against `bb48_drive` in PIO, the hard disk alone.** That's what retro PCs use, and where core 1's margins are thinnest.
- **RAM:** `pcb_drive` built with `-DHDDPRO_HDCD=1` uses 94.1% (493,264 of 524,288 bytes), the same as `bb48_hdcd`; `bb48_drive` and `pcb_drive` use 91.7%. `check_core1_ram.py` passes on it (69 functions reachable from `loop1`).

## Step 1: measure (the bench, about an hour)

The decision this step makes: is `hdcd::Device`'s extra branch per access free in PIO for the hard disk alone?

On the PCB or the BB48, with the USB adapter on Windows (`scripts/rig.sh` from Git Bash, in an administrator terminal, with `PIO_ENV` set; replugs by hand, see `CLAUDE.md`), on the built-in test disk (console `T`; never an image for slices 3-4). Run each build twice, alternating A-B-A-B, since results drift by a few per cent between runs:

1. `PIO_ENV=bb48_drive`, `HDDSYNTH.INI` with `device1 = none`, `mode = auto` (PIO 3), early answers on. Record:
   - `-m speed`: MB/s, IOPS and the worst read;
   - slices 1-4;
   - console `s` after `r` and a speed run: `iordy_max_ns`, `iordy_slow`, `iordy_data_slow`, `iordy_data_max_ns`, `iordy_ignored`.
2. `PIO_ENV=bb48_hdcd`, the same settings (no CD-ROM): the same records.
3. Both again with `early_answers = off` (the data lane), which is fastest on PCI PCs and leaves core 1 the least slack: `-m speed` and the `s` fields.

**Decision rule:**
- **Merge** if `bb48_hdcd` is within the run-to-run spread on MB/s and IOPS, and its worst reads and `iordy_data_slow` buckets aren't worse.
- **Otherwise** keep a hard-disk-only build per board (four builds: `bb48`, `bb48_drive`, `pcb_drive`, plus a combined `*_hdcd` for each board) and stop at the CD-ROM-on-SDIO part of step 2.

Write the figures into `docs/speed-log.md` and the CHANGELOG either way.

## Step 2: the code (no hardware)

1. **The CD-ROM as device 0 in `hdcd::Device`.** Today `bb48_cdrom` exists only because `hdcd` can't put the CD-ROM on device 0. Suggested: a setting `device0 = cdrom` (or a name ending in `.ISO` or `.CUE`), with the disc from `cdrom` as today. `hdcd::Device` then holds the CD-ROM on unit 0 and nothing (or a hard disk) on unit 1. The cheapest change to the per-access path is to store which unit the CD-ROM is on and compare: `cd_on_ && ((device_reg_ & DEV_DEV) != 0) == cd_unit_`. That costs one compare more than today: measure it as in step 1 (rerun `-m speed` on both). Keep `test_hdcd.cpp` passing, and add tests for the CD-ROM on unit 0 (`test_atapi_device.cpp` holds the CD-ROM's own behaviour).
   - Check what `bb48_cdrom` does that `hdcd` doesn't yet: the `#if HDDPRO_CDROM` sections in `main.cpp`, for example IDENTIFY for the bus observer (`bus_observer.cpp`, `buildIdentifyPacket` for device 0), `g_disk_chosen`, the disc as device 0 in `chooseDisk`, console `M`, and the READY line. Carry each over as a runtime choice on the CD-ROM's unit.
2. **Make the combined device the only drive device:** `drive_device.h` always picks `hdcd::Device`. Remove `HDDPRO_CDROM` and `HDDPRO_HDCD`, and the `bb48_cdrom` and `bb48_hdcd` environments, after which `bb48_drive` and `pcb_drive` carry everything. `HDDPRO_PCB` stays: it picks SDIO (`config.h`'s `SD_SDIO`).
3. **SDIO with a CD-ROM on the PCB:**
   - `sd_card.cpp` parks the card when nothing on it is served (`sharePio()`). It checks `s_image`, `s_device1_image` and a served folder; check a disc on device 0 or 1 counts as served.
   - `pio_plan::choose()` doesn't depend on the devices, but with a CD-ROM `ide_bus` is forced, which doesn't change the PIO blocks.
4. **Tests and tools that know about builds:**
   - `host/conftest.py`'s `is_cdrom_build()` and `serves_cdrom()` read the READY line's ` cdrom` / ` hdcd` suffix. Make them ask the drive's settings instead (`S`: `CONFIG device1=cdrom kind=cdrom ...`, and the device 0 equivalent you add).
   - The READY line in `main.cpp` (`READY HDDSynthPro <version> slice3[ cdrom| hdcd]`) and `host/hddproto/trace.py` (`CLAUDE.md`: change the parser and the format together).
   - `.github/workflows/build.yml`: the build list, the core 1 checks, and the release files (drop `bb48-cdrom` and `bb48-hdcd`).
   - `CLAUDE.md` (the build list, *Toolchain*), `docs/atapi.md`, `docs/settings.md`, `welcome/README.TXT` (the welcome disk's settings table), and the release notes.
5. **Core 1 rules:**
   - Everything per access stays `CORE1_RAM` / `always_inline`, and `scripts/check_core1_ram.py` must pass on both drive ELFs.
   - Don't add `if (config)` branches to `loop1` beyond the device's own `cdSelected()`.
   - Benchmark reads after any core 1 change (`CLAUDE.md`, *Core split*).
6. **Versioning:**
   - `0.32.0-dev.N` while on the branch, a CHANGELOG entry for each firmware change, and `0.32.0` to release.
   - The release notes go in `docs/release-notes/v0.32.0.md`; CI uses that file as the release text.
   - **Settings:** `HDDSYNTH.INI` files from 0.31.0 must keep working: `device1 = cdrom` as today, and `cdrom = NAME` as the disc.

Host tests: `test/run_native_tests.sh` needs WSL on Windows (or rely on CI, which runs it with ASan/UBSan); `cd host && pytest` runs the simulations.

## Step 3: check on the board, then release

On the adapter (Windows), with the merged `bb48_drive` and `pcb_drive`:
- the hard disk alone: slices 1-4 and `-m speed` in PIO and in MDMA 2 (`mode = mdma2`), compared with step 1;
- a CD-ROM as device 1 (`device1 = cdrom`, `mode = mdma2`): `HDDPRO_CDROM_DEV=//./CdRom0 pytest -s -m cdrom`, and a disc change (`M`);
- a CD-ROM as device 0 (`device0 = cdrom`, `device1 = none`): the same;
- on `pcb_drive`: `S` says `bus=sdio`, and a short card soak (`-m cardsoak`, 30 minutes, on `HDDBENCH.IMG` with console `J`);
- an hour's soak of each drive build on the test disk with full observer checks (`HDDPRO_SOAK_MINUTES=60 pytest -s tests/test_soak.py`; it's under the `slice6` marker with other tests).

Then: CHANGELOG `## [0.32.0]`, the release notes, fast-forward `main`, tag `v0.32.0`, and delete the branch.

## Things to know on the rig

- **The card's SPI trap:** any SPI build (`bb48_*`) leaves the card in SPI mode until its power is cut. After one, `pcb_drive` runs the card over SPI with a warning in `S`; take the card out and put it back, and it restarts onto SDIO by itself.
- **The adapter and a CD-ROM:** after a flash, restart or console `V` with a CD-ROM as device 1, the adapter went silent until it was replugged (4 Nov). Replug it after every flash of a build serving a CD-ROM.
- **Mounted images:** Windows may mount a FAT image the drive serves. Use the test disk (`T`) or `HDDBENCH.IMG` (`J`) for anything that writes; never format or write to the owner's images (`CLAUDE.md`, *Drive data path*).
- **The card's settings now (9 Nov):** `device0 = ICL386MB.IMG`, `device1 = cdrom`, `cdrom = DOOM_SE.ISO`, `mode = auto`, `early_answers = off` (the owner's choice for the Evesham). Put them back after the bench work.
- **The board** runs `pcb_drive` 0.32.0-dev.5 (10 Nov), the card on SDIO, serving the test disk (`T`) with `device1 = none`, `mode = mdma2`, `dma = on`: put the owner's settings above back when the bench work is done (`device1 = cdrom`, `mode = auto`, `dma = off`, then `T` again to leave the test disk).
