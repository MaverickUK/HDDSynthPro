# Next bench session (rewritten 10 Nov 2026)

What's left needs the rig: the Windows PC (an administrator Git Bash), the USB adapter (JMicron `152D:2338`) and the PCB on COM4, from the `one-build` worktree (`C:\Users\Peter\Documents\Code\HDDSynthPro-onebuild`), in `host/`. The rig's quirks are in `CLAUDE.md` (*Rig*). Older checklists are in this file's git history.

## 1. Bring the rig back

- [ ] If the adapter has been declining (it garbles its own USB serial, `984220294922`, into variants like `084200294902`), **restart the PC** first.
- [ ] **Unplug the adapter and plug it back in.** The board waits as left: `pcb_drive` 0.32.0-dev.5, the test disk (console `T`), `device1 = none`, `mode = mdma2`, `dma = on`. Windows should show `HDDSYNTH PROTO` as disk 1.
- [ ] The card on SDIO: `S` says `bus=sdio`. If it says SPI, take the card out and put it back.

## 2. Unattended: `bash runs/bench/after_replug.sh` (about 75 minutes)

It runs, in order:
1. slices 1-2 (20 re-detections each, `HDDPRO_REPLUG=redetect`);
2. a 30-minute card soak on `HDDBENCH.IMG` over SDIO (console `J`, then `J` back);
3. the owner's settings back (`device1 = cdrom`, `mode = auto`, `dma = off`), the build flashed with the access log (`-DHDDPRO_ACCESS_LOG=1`), and three re-detections captured (`runs/bench/acclog-dev5.txt`);
4. the normal `pcb_drive` flashed back.

Results: `runs/bench/slices12-dev5b.txt`, `cardsoak-dev5b.txt`, `acclog-dev5.txt`. With the CD-ROM configured and DMA off, the adapter can't use the CD-ROM (it sends every packet by DMA) and resets the drive every 20 s: expected, and handy for the capture.

## 3. The CD-ROM's slow register reads ([atapi.md](atapi.md#slow-device-reads-on-ide_bus-10-nov-2026-0320-dev5))

Done 10 Nov: core 1 isn't late, and neither fix helps (CHANGELOG, 0.32.0-dev.7). Kept for the record:
- [x] Read `acclog-dev5.txt`: each `LATE answer` block lists what core 1 did in the microseconds before a read it answered late (accesses, commands, long loop steps, with times). Which job takes the time before the slow Device reads?
- [x] If it's the register writes (both devices' `write` per access), compare the candidate fixes (`HDDPRO_HDCD_FAST_WRITES`, `HDDPRO_HDCD_LAZY_INTRQ` in `src/hdcd_device.h`): `bash runs/bench/cd_fixes.sh` flashes none, each, then both, each with the access log, **asks for a replug after each flash** (four, plus one at the end), captures two re-detections each and prints `iordy_max_ns`, `iordy_slow` and the late answers per variant (`runs/bench/cdfix-summary.txt`). About 25 minutes with the owner there.
- [ ] Then the best variant with a normal `-m cdrom` run (`mode = mdma2`, `dma = on`, `T`; `MSYS_NO_PATHCONV=1 HDDPRO_CDROM_DEV=//./CdRom1`; the number changes as Windows re-enumerates).
- [ ] A fix that helps becomes the default only after the hard disk's code is shown unmoved (`scripts/check_core1_layout.py`) and the CD-ROM passes `-m cdrom` and an hour's soak.

## 4. The release ([one-build-per-board.md](one-build-per-board.md), *The release, step by step*)

- [ ] Slices 1-2 and the card soak in the release notes' *Tested* section.
- [ ] With the owner's go: version `0.32.0`, the CHANGELOG entry, fast-forward `main`, tag `v0.32.0`.

## 5. Retro PCs

- [ ] **Every PC, every build:** the table in [auto-bus.md](auto-bus.md#what-to-record-on-each-pc), the evidence for the drive choosing its own bus program. At three moments (the BIOS has found the drive, the DOS prompt, after a benchmark; `r` before each): `bus`, `dior_no_cs` and the `NOCS` line, `iordy_ignored` and any `NOIORDY` lines, `iordy_slow` and `iordy_max_ns`, `rd_t1_ns`, `rd_strobe_ns`, with the controller type. ISA cards should show millions, PCI controllers 0. **Never `P l` (the data lane) on an ISA machine.**
- [ ] **The Evesham, MS-DOS 5 from C: with the CD-ROM driver** (from 4 Nov): boot the Windows 98 startup floppy, `MD C:\CDROM`, copy `OAKCDROM.SYS` (from A:) and `MSCDEX.EXE` (from the floppy's RAM drive) there, add `DEVICE=C:\CDROM\OAKCDROM.SYS /D:MSCD001` to `C:\CONFIG.SYS` and `C:\CDROM\MSCDEX.EXE /D:MSCD001` to `C:\AUTOEXEC.BAT` (a version check may need `SETVER`). Boot from C: and run Doom from the CD. This writes to `ICL386MB.IMG`: only with the owner there.
- [ ] **If a BIOS doesn't find the drives** with a CD-ROM configured: console `P e` (early answers; restarts), the program each drive ran alone on the Evesham, and say which worked. `P 0` goes back to the settings'.

## 6. Put the card back

- [ ] The owner's settings (for the Evesham): `device0 = ICL386MB.IMG`, `device1 = cdrom`, `cdrom = DOOM_SE.ISO`, `mode = auto`, `dma = off`, `isa = on`, `early_answers = off`. Console `E key = value`, then `!`; `S` to check. Only `HDDSYNTH.INI` changes; never format the card.
- [ ] No console `T`, `J` or `P` left set (`S`: `serving=ICL386MB.IMG`, no `CONFIG bus program set from the console`).
- [ ] A normal build flashed (no `RIG_WATCHDOG_MS`, no `HDDPRO_ACCESS_LOG`) before the board goes anywhere else.
