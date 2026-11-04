# Next bench session (written 4 Nov 2026)

Everything built on 4 Nov while nobody could replug the USB adapter, in one sitting. Branches: `merge-cdrom` (0.30.0-dev.3, CI green) and `hdd-cd` (0.30.0-dev.8, from it; everything below is on it). Run from an administrator Git Bash on the Windows PC, in `host/`, with `export PYTHONUTF8=1 HDDPRO_DISK=PhysicalDrive1`. `runs/tools/` holds the helpers named below (git-ignored, on that PC).

**Steps 1, 2 and 2b run by themselves:** after step 0, `python bench_session.py` in `host/` flashes each build, sets the console up, re-detects, runs the tests and prints a summary beside the figures below (`--list` for the steps, `--only`/`--from` to pick, `--dry-run` to see the plan; results in `runs/bench-<time>/summary.md`). It restores `device1` and `mode` at the end. What stays by hand: the replug, MSCDEX and the retro PCs, the soak (`--only hdcd-soak` runs it), step 5's flash.

## 0. Bring the rig back

- [ ] **Replug the USB adapter** (it dropped off USB on 4 Nov: its controller hung, no probing on the IDE side). If it misbehaves again (garbled names, a disk of the wrong size, double detections in most cycles), **restart the PC** before blaming the firmware; it has been declining for weeks.
- [ ] Power-cycle the BB48 too: that clears the console's `T` (test disk) and `P l` (data lane) and unsticks the SD card if a warm restart left it stuck (SdFat code 23).
- [ ] The board runs `bb48_drive` 0.30.0-dev.7 built with `RIG_WATCHDOG_MS=8000`, a rig-only build. Console `h` shows the READY line.
- [ ] `git checkout hdd-cd && git pull` (it holds everything in `merge-cdrom` too).

## 1. PIO after the DMA fix (0.30.0-dev.3), on `bb48_drive`

dev.3 changed early answers' tight loop (DMA now runs from its idle work). DMA was measured afterwards, PIO wasn't.

- [ ] Console `T` (test disk), `P l` (lane), then `runs/tools/redetect.py` and `powershell -File runs/tools/usb_restart.ps1`: `pytest -s -m speed`. Expect about 1.54 MB/s reads, 80 IOPS, 1.68 MB/s writes (dev.2).
- [ ] `P e` (early answers), the same: expect about 1.21 MB/s, 76 IOPS, 1.13 MB/s (dev.2).
- [ ] Slices 3-4 on the lane: `pytest -s -m "slice3 or slice4"`.

## 2. A hard disk and a CD-ROM: `bb48_hdcd`

- [ ] `PIO_ENV=bb48_hdcd`, or flash it: `PLATFORMIO_BUILD_FLAGS=-DRIG_WATCHDOG_MS=8000 pio run -e bb48_hdcd -t upload`. READY ends `slice3 hdcd`.
- [ ] Console `E device1 = cdrom`, `E mode = mdma2` (this adapter sends every CD-ROM packet by DMA), `T` (test disk and test disc), then `S`: `CONFIG device1=cdrom kind=cdrom disc=test disc frames=32768`.
- [ ] Re-detect (`runs/tools/redetect.py`; with Windows still holding a CD from an earlier build, `redetect.py //./CdRom0`), then `usb_restart.ps1`. Windows should show the disk **and** `HDDSYNTH PRO CD-ROM` as a CD drive.
- [ ] `pytest -s -m speed` (device 0, now in DMA): compare with `bb48_drive` in DMA on early answers, 8.84 MB/s reads, 1,000 IOPS, 12.48 MB/s writes. **This prices the extra branch per access**: if it's within noise, `bb48_hdcd` could become the only drive build.
- [ ] `HDDPRO_CDROM_DEV=//./CdRom0 pytest -s -m cdrom` (device 1): every byte of the test disc. Compare its MB/s with dev.3's 2.42 MB/s, **the first measure of CD-ROM read-ahead** (dev.5).
- [ ] Interleave both (a speed run on the disk while `-m cdrom` reads the disc, in two terminals): no errors, `bus_bad=0`.
- [ ] Disc changes (console `M`, dev.7): with Windows showing the CD drive, `M none` (Windows should show it empty), `M DOOM_SE.ISO` (Windows should mount `DOOM_SE` without a restart), `M test`. Then on the Evesham, in DOS with MSCDEX: `DIR D:`, `M test`, `DIR D:` again (does MSCDEX notice?).
- [ ] `T` off: `ICL386MB.IMG` as C: and `DOOM_SE.ISO` on the CD. Windows mounts `DOOM_SE` (CDFS); read every file (dev.3: 27 files, 22.7 MB, 1.36 MB/s without read-ahead).

## 2b. The CD-ROM build's slow reads ([atapi.md](atapi.md#the-cd-builds-slow-reads-what-the-code-says-4-nov-2026-0300-dev6))

- [ ] Flash `bb48_cdrom` with `PLATFORMIO_BUILD_FLAGS="-DRIG_WATCHDOG_MS=8000 -DCMD_PROFILE=1"`, `E mode = mdma2`, `T`, re-detect. `r`, then `HDDPRO_CDROM_DEV=//./CdRom0 pytest -s -m cdrom`, then `s`: note `iordy_max_ns`, `iordy_slow`, `cmd_run_max_ns` and `cmd_run_max` (command/packet).
- [ ] The same with `P i` (`ide_bus`), then `P e` back. Which program has the fewest reads past 1,250 ns, and does `cmd_run_max_ns` account for them?
- [ ] If `cmd_run_max_ns` is well over 1 µs, try the candidate fix: build with `-DEARLY_POLL_FIRST_NS=1000` added (the host's first poll answered, BSY, before the packet runs), early answers, the same run. Fewer reads past 1,250 ns, and no errors? Then it can become the default.

## 3. A short soak

- [ ] On `bb48_hdcd` with the CD-ROM in (test disk and disc): `HDDPRO_SOAK_MINUTES=60 HDDPRO_SOAK_TOLERATE_RESETS=1 HDDPRO_SOAK_IORDY_MAX_NS=2000 pytest -s -m slice6`.

## 4. Retro PCs

- [ ] **Every PC, every build:** note `dior_no_cs` in `s` and the `NOCS` lines (0.30.0-dev.2) with the controller type. ISA cards should show millions, PCI controllers 0. With enough machines, the drive could choose its bus program by itself.
- [ ] **The Evesham on `bb48_hdcd`:** `device0 = ICL386MB.IMG`, `device1 = cdrom`, `cdrom = DOOM_SE.ISO`. Does the BIOS list a hard disk and a CD-ROM? MS-DOS 5 from C:, then a CD-ROM driver (`OAKCDROM.SYS`, MSCDEX) finding the drive as device 1, and Doom from D:. PIO there (`mode = auto`), as its DOS driver uses.
- [ ] The folder drive's `CHECK.BAT` (two drives, still to do), and the data lane on another PC.

## 5. Put the card back

- [ ] `E device1 = none`, `E mode = auto` (and `E early_answers = on`, `E isa = off` if changed), `!`. Only `HDDSYNTH.INI` changes; never format the card.
- [ ] Flash a normal build (without `RIG_WATCHDOG_MS`) before the board goes anywhere else.
