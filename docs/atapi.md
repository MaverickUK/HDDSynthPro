# ATAPI CD-ROM: plan for a first exploration

28 Oct 2026, planned. Branch `atapi-cdrom`.

## One build since 0.32.0 (9 Nov 2026, [one-build-per-board.md](one-build-per-board.md))

The CD-ROM builds are gone: `bb48_drive` and `pcb_drive` hold the CD-ROM drive, chosen in `HDDSYNTH.INI` ([settings.md](settings.md)):

| Up to 0.31.0 | Since 0.32.0 |
|---|---|
| `bb48_cdrom`: a CD-ROM drive alone, device 0 | `device0 = cdrom` (or an `.ISO`/`.CUE` name), `cdrom = DISC.ISO` |
| `bb48_hdcd`: a hard disk, and a CD-ROM as device 1 | `device1 = cdrom` (or an `.ISO`/`.CUE` name) |

Both are `hdcd::Device`, which keeps the CD-ROM on unit 0 or 1 (`cdSelected()` is one mask and one compare for either, or for none). With a CD-ROM, as device 0 or 1, the drive runs `ide_bus`, never the data lane or early answers (console `P e` still picks them): on early answers `hdcd::Device` answers Status in 1.8-2.4 µs and the USB adapter never detects the drive (`bb48_cdrom`'s own `atapi::Device` had managed there). The hard disk alone runs a loop of its own, compiled for `ata::Device` (0.32.0-dev.2), so a CD-ROM costs it nothing. The READY line no longer names a build (`READY HDDSynthPro 0.32.0 slice3`); `S` says where the CD-ROM is: `CONFIG device0=cdrom kind=cdrom disc=DOOM.ISO frames=N` (or `device1=`). The PCB serves discs over SDIO, as it serves images.

The sections below are the history, with the build names of their time. To try the CD-ROM now: `PIO_ENV=bb48_drive scripts/rig.sh -m cdrom` with `E device0 = cdrom` (or `device1`), `E cdrom = none` and `!` (or `T`) for the test disc; on Windows `HDDPRO_CDROM_DEV=\\.\CdRom0` and `mode = mdma2`.

## A hard disk and a CD-ROM together: `bb48_hdcd` (4 Nov 2026, 0.30.0-dev.4, branch `hdd-cd`)

**Through the USB adapter and on a retro PC (4 Nov, dev.10).** On the adapter: both drives in Windows, soaked an hour ([CHANGELOG](../CHANGELOG.md), dev.10). **On the Evesham** (Athlon XP, PCI IDE, `ide_bus`, PIO 3): the BIOS and the Windows 98 floppy's `OAKCDROM.SYS`/MSCDEX found both, C: as `ICL386MB.IMG`'s MS-DOS 5 and D: as the CD-ROM; Doom ran from `DOOM_SE.ISO` with its saves on C:, 3,361 commands, none failed, 5,360 sectors checked on the cable, 0 bad. The host didn't wait for IORDY on 26 CD data reads (none on the disk's), and held Status reads up to 3.6 µs (the slow reads below). Still to try: MS-DOS 5 booting from C: with the CD-ROM driver on it (`docs/bench-checklist.md`).
- **Settings:** `device1 = cdrom` (the `cdrom` setting's disc) or a device1 name ending in `.ISO`. The disc follows the CD-ROM build's rules: the ISO; the test disc after console `T` or with `cdrom = none`; no disc if the file isn't there. Without such a `device1` the build serves what `bb48_drive` does, a second disk or folder included.
- **How:** `hdcd::Device` (`src/hdcd_device.*`) holds an `ata::Device` as device 0 and an `atapi::Device` at position 1, and behaves as two devices on one cable: both take every register write and RESET#; the selected one answers reads, runs commands and drives INTRQ; DMA goes to whichever runs it. `atapi::Device` learned its position: it answers when DEV is 1, its fetches carry unit 1 (core 0 reads device 1's disc, as two drives does) and ids with the top bit set, so each result reaches the device that asked; DEVICE RESET leaves it selected. With a CD-ROM it runs on `ide_bus` (0.30.0-dev.10): on early answers the USB adapter never got past its register check after RESET# and never detected the drive, and the data lane was written for the hard disk's blocks. Through the USB adapter it needs `mode = mdma2`, as the CD-ROM build does (the adapter sets the DMA bit on every PACKET).
- **A build of its own** for now: every access takes one more branch (which device is selected), so `bb48_drive` stays the hard disk build, its core 1 code unchanged, until the bench says whether that branch costs anything. RAM: 8.9 KB more than `bb48_drive` (the CD-ROM device, its buffers and its code in RAM), which leaves less heap for a folder as device 1, but a folder and a CD-ROM can't both be device 1 anyway.
- **Tests:** `test_hdcd.cpp` (a BIOS probe finding both, signatures after power-on, RESET# and SRST, interleaved reads from both with each fetch from its own disk, INTRQ following the selection, DEVICE RESET to the CD-ROM alone, EXECUTE DEVICE DIAGNOSTIC on both, a stale CD-ROM fetch never reaching the disk, DMA on either, and without a CD-ROM the hard disk build's two drives), and `drive_config`'s `device1IsCdrom`.
- **For the bench:** through the USB adapter (both drives listed; `-m speed` on device 0 against `bb48_drive`, to price the branch; `-m cdrom` on device 1), then a retro PC booting from the disk with the CD-ROM driver finding device 1.

## Read-ahead (0.30.0-dev.5)

Disc reads now fetch the next 512-byte sector into a second buffer while the host takes the one before, as the hard disk does (`read_ahead`, console `A`): host-tested (`atapi_read_ahead_*`: one BSY wait for a two-frame read instead of eight; a read-ahead that fails reports MEDIUM ERROR after the sector before; one cut short by a reset is stale). Not yet measured: the 1.36 MB/s Windows read the Doom disc at (dev.3, no read-ahead) is the figure to beat.

## BIN/CUE images (0.30.0-dev.9)

`cdrom = NAME.CUE` (or `device1 = NAME.CUE`, or console `M NAME.CUE`) serves a CUE sheet's first track from the BIN it names, both in the card's root folder: the drive reads each 2,048-byte frame out of the BIN's raw ones (2,352 bytes: sync, header, data from byte 16 in MODE1, 24 in MODE2 form 1; EDC/ECC never read). The data track ends where the next track in its file starts (its INDEX 00, the pregap), else at the file's end, so a mixed-mode game (data, then its music as audio tracks) serves its data; the audio isn't served, since the drive has no audio output. MODE1/2048 (a "cooked" BIN, an ISO by another name), MODE1/2352, MODE2/2352 and MODE2/2336; an audio CD, or a first track of another kind, is refused with the reason (`CONFIG cdrom not used: GAME.CUE: the first track is audio: no data to serve`).

`src/cue_sheet.*` reads the sheet (its first 1.5 KB: track 1 and where track 2 starts are always near the top) and `RawTrackDisk` maps the drive's 512-byte sectors into the frames; host-tested (`test_cue_sheet.cpp`: one-file and one-file-per-track sheets, every mode, BOMs, folders in names, refusals, and every byte of a raw track). Core 0 only: core 1's code is unchanged (the hard disk build's at the same addresses). The BIN's frames aren't 512-byte aligned, so reads go through SdFat's file layer, not the extent map: slower per sector than an ISO, not yet measured. `tools/iso_to_bincue.py` makes a BIN/CUE from an ISO (`--audio` adds a silent audio track) to test with; a C++ check read its four kinds back through `RawTrackDisk` identical to the ISO. **Not yet tried on the board.**

**On the board (4 Nov, the Evesham):** `DOOMBIN.CUE`, made by `tools/iso_to_bincue.py DOOM_SE.ISO --audio 30` (MODE1/2352, then a silent audio track), loaded with console `M` as `DOOMBIN.BIN, track 1 of 2 (2352-byte frames from byte 16), 11431 frames`; MSCDEX listed `DOOM_SE`'s files from it, no errors. A first try failed every read (MEDIUM ERROR) because the SD card had dropped out and device 1's file reads don't recover from that (`CHANGELOG.md`, dev.10); after a power cycle it read cleanly.

## Changing discs (0.30.0-dev.7)

Console `M`, then a disc and Enter, in both CD-ROM builds: an ISO or a CUE sheet on the card, `test` (the generated test disc) or `none` (eject), until a restart (`HDDSYNTH.INI` isn't changed). The host sees what a real drive's tray would show: a media-removal event (GET EVENT STATUS NOTIFICATION) and NOT READY while it's out, then a new-media event and a unit attention, medium may have changed (28h), on its next command. Core 0 ejects first and waits for core 1 to take the eject, which it does only once the device is idle (no command running, no fetch outstanding); only then does it swap the file and insert the new disc, so no fetch for one disc is ever served from the next. Host-tested (`mmc_disc_changes`, `atapi_changing_the_disc`, `hdcd_changing_the_disc_leaves_the_disk_alone`); on the board, swaps between `DOOM_SE.ISO`, the test disc, none and a missing file report as they should (`DISC DOOM_SE.ISO frames=11431`). Both Windows (through the USB adapter) and MSCDEX on the Evesham (4 Nov) notice: MSCDEX answered `CDR101: Not ready` with the tray empty, re-read the TOC and sector 16 after the unit attention, and said `CDR103: CDROM not High Sierra or ISO-9660 format` for the test disc rather than listing the old disc from its cache. Next: the HDD Synth's buttons, and a list of discs to step through for multi-disc games.

## A wrong data word on the Evesham: Doom quits (10 Nov 2026, 0.32.0)

*Found on 11 Nov: the IORDY watchdog, started by mistake for the CD-ROM, released IORDY mid-read. Fixed in 0.32.1-dev.5 (below, "What the Evesham showed").*

**The fault:** on the PCB in the Evesham (PIO 3, `ICL386MB.IMG` as C:, `DOOM_SE.ISO` on the CD-ROM as device 1, `ide_bus`, the Windows 98 startup floppy and MSCDEX), Doom's demo ran for over an hour and then quit with `R_TextureNumForName: GST0T01 not found`. The bus observer, on full checks, flagged one sector at the same moment: `BUSERR lba=17366 word=93 seen=4f54 want=454e bad_words=1`. That's frame 4341, block 2. The ISO there holds `GSTONE1` twice. Word 92 is `TO` (4f54) and word 93 `NE` (454e), and the cable carried `TO` for both. So Doom read `GSTOTO1` (`GST0T01` on screen) and couldn't find the texture.

**What the evidence says:**
- **The drive's word sequence stayed in step.** The words before and after were right, and only one word in the sector was wrong. The host's strobe 93 found the previous word's value on DD, and strobe 94 found word 94.
- **The card isn't the cause.** It read the sector right: the observer's expected data comes from core 0's fetch, and it matches the ISO on the Mac.
- **This explains the evening's other failures.** On 0.32.0-dev.6 there had been errors on D: and Doom didn't start. On 0.32.0, over SDIO, the PC froze with black and white lines over part of the screen, the drive still waiting in the middle of a READ(10) (Status 48h, data offered and never taken; nothing on the bus after it). A `COMP` of `D:\DOOMCD` against a copy on C: (13 MB, read twice) found no difference, as you'd expect from a fault this rare.
- **It's rare:** 1 bad word in 11,294 sectors checked, about 5.8 million words.
- **No diagnostic saw the host skip the wait.** `iordy_ignored=0`, `iordy_data_max_ns=1110`, no `NOIORDY`, no card lost or restarted. The log is `host/runs/retro-20261110-220709-evesham-pcb-doom-spi-observer.log`, on `bb48_drive` with the card on SPI so that the observer has its block.

**How it can happen (not proven):** between reads `ide_bus.pio` releases DD (`mov pindirs, null`), and the undriven lines keep their last value. So the host read word 93 before the drive drove it, which means the host didn't wait for IORDY on that read. Two candidates:
1. **IORDY is late for back-to-back data reads.** After a read the program releases IORDY at `wait 1 jmppin side 0` and pulls it low again only at `arm`, about 7 cycles after it sees DIOR# rise: 35 ns, plus the input synchroniser, so about 45-55 ns. ATA's tA is 35 ns after the next strobe's start, so a controller that runs its recovery (t2i) shorter than PIO 3's 70 ns could sample IORDY high on rare reads, and not wait.
2. **This controller doesn't always sample IORDY on the data port** (the 4 Nov guess below, which the PCB's clean 9 Nov run seemed to rule out).

Either way `iordy_check.pio` should have caught it: the host's strobe would end while IORDY was low, before core 1 answered. Unless core 1 answered within the host's strobe but after its data sampling, which is the case it can't see, or a DIOR# glitch did something else.

**What the simulator says (11 Nov, `host/tests/test_stale_word_sim.py`):** neither candidate fits.
- **(1) is ruled out:** `ide_bus` pulls IORDY low again 25-30 ns after DIOR# rises, not the 45-55 ns guessed above. So IORDY is low at the next strobe's tA with recovery down to 20 ns.
- **A host that skips its wait gets flagged:** with `iordy_check` and the bus observer running beside `ide_bus` on the same pins, every data read the host didn't wait for and read wrong was flagged. That's 1,165 cases, over core 1 latencies of 40-300 ns, strobes of 60-300 ns, recovery of 10-70 ns, undriven DD lines keeping their last word, and IORDY rising over up to 25 ns. So (2), as the model knows it, doesn't fit `iordy_ignored=0` either.

**What fits the evidence:**
- The observer saw `TO` on the cable within 20 ns of DIOR# rising, and the words before and after were right.
- `iordy_check` saw IORDY high just after the strobe. The program releases IORDY only 40 ns after driving DD, so the drive had been driving DD for at least that long.

So either:
- **Electrical:** DD didn't take the new word in 40 ns or more, or something cut the strobe short at the moment DD was driven. For example, simultaneous switching noise briefly lifting the IORDY low the drive holds, so that the host ended the strobe within a few tens of ns.
- **The drive offered `TO`:** core 1's word for that read was word 92 again, from a sector buffer changed under it. Reading the code, no path of core 1's answers a data read with a stale word and still steps past it (the direct answer, `serveOther`'s catch-up, a sample with DIOW# or the address misread). Each gives the right word or shifts the rest, and the rest wasn't shifted. A buffer written under core 1 is still open.

**The measurement that decides:** 0.32.1-dev.1's `-DHDDPRO_OBSERVER_STROBE=1` makes the bus observer time each read's strobe, and `BUSERR` report the wrong word's against its neighbours'. A short one (tens of ns, against hundreds) is the electrical case; one as long as the rest is the drive offering the wrong word. The board was left on `bb48_drive` built with it (the card on SPI, so the observer has its block). Next on the Evesham: Doom's demo with the observer on full checks (console `O`) until a `BUSERR`, then read its `strobe_ns`.

**What the Evesham showed (11 Nov, 0.32.1-dev.1-5): the IORDY watchdog.** Doom's demo with the observer on full checks and the strobe timed, one build per run (logs `host/runs/retro-20261111-*-evesham-*.log`):

| Build | What changed | Sectors checked | Bad words |
|---|---|---|---|
| dev.1 | strobe timing only | 20,344 | 2 |
| dev.2 | DIOR# through the input synchroniser | 14,432 | 1 |
| dev.3 | DD at 8 mA | 20,545 | 1 |
| dev.4 | `EARLYDATA`: data reads that start with IORDY high | 19,284 | 1 (no `EARLYDATA` near it) |
| dev.5 | no watchdog (the fix) | 41,640 | 0 |
| dev.6 | the watchdog restarted at every strobe (`isa = off`, so it doesn't run here) | 40,536 | 0 |

Every bad word looked the same:
- **The cable held the previous word, with DD7 low:** `01a8` for `0190`, `fea0` for `fce8` (both read with bit 7 clear), `494e`, `0001`, `524f` (the previous word itself, its DD7 already 0). That's DD left undriven: the lines keep their last level, and the host's pull-down drains DD7. None of the lines that should have changed had moved, at 4 mA or 8 mA. The 10 Nov word fits too.
- **The strobe was 225 ns, the shortest in its sector**, against 345-465 ns either side: the host's own minimum, not waiting on IORDY.
- **IORDY was low as the read started** (no `EARLYDATA`) and high as it ended (`iordy_ignored=0`). Only one word was wrong, so core 1 served it once.
- **Doom read what the observer saw:** the dev.4 run's bad word (`OR` for `TR`, sector 13998) made it quit with `R_TextureNumForName: DOORORAK not found` (`DOORTRAK`).

So something released IORDY mid-read before core 1's answer, without driving DD: `iordy_watchdog.pio`. A CD-ROM forced `isa` on, only to keep `ide_bus` rather than the data lane, and `isa` also starts the watchdog, although `HDDSYNTH.INI` said `isa = off` and the Evesham is a PCI host. The watchdog releases IORDY once it has been held 23 µs, and it starts its count again only when it sees IORDY high, checking once every 9 cycles (45 ns). In a run of data reads IORDY is high at the pin only for a moment between reads (the host's pull-up raises it through the bus switch, and `ide_bus` pulls it low again about 25 ns after DIOR# rises). Miss those moments for 23 µs and the watchdog takes back-to-back reads for one long hold and releases IORDY in the middle of one. The host then ends the read at its minimum and latches DD before the drive has driven it; core 1's word goes onto the bus after the strobe, so the sequence stays in step.

**The watchdog's other trace:** `iordy_early`, reads that start with IORDY still high, almost all Status. Each run up to dev.4 counted 9,698-13,880 of them, the watchdog having released IORDY while the drive sat selected between accesses. dev.5 counted none in 36,456 sectors. CLAUDE.md's note that in PIO mode 3 "most" reads start early came from runs with the watchdog on, it seems.

**The fix (0.32.1-dev.5):** a CD-ROM keeps `ide_bus` through `ide_bus::begin`'s new `lane` argument and leaves `isa` as `HDDSYNTH.INI` sets it, so the watchdog (and IOCS16#) runs only with `isa = on`.

**Still open:** the watchdog has the same weakness wherever it runs. An ISA host fast enough to put data reads back to back with IORDY high only briefly between them would hit it too. The ICL 386 (PIO 0, slow) hasn't. A sturdier watchdog would start its count again at every strobe (DIOR# or DIOW# low), not only when it sees IORDY high. That's written (0.32.1-dev.6, left out of v0.32.1; since 14 Nov on branch `iordy-watchdog-strobes`, 0.32.3-dev.2): it passes in simulation, but waits for the ICL 386 (PCB rev 0.4, which wires IOCS16#).

**On the bench:** `-m cdsoak` reads the test disc in Doom's pattern for an hour with every sector checked. But through the rig's adapter the CD's data goes by multiword DMA, not `ide_bus`'s PIO data path, so it soaks the CD-ROM's commands and the card, not this fault. A host that reads a CD-ROM by PIO is what reproduces it.

## The Evesham's unwaited CD data reads (4 Nov 2026)

**Update, 9 Nov 2026: not on the PCB.** The same session on the IDE Synth Bay PCB (`bb48_hdcd` 0.31.0, same core 1 code, same PC, same game, same settings) had **no** unwaited reads: 2,966 READ(10), 0 `NOIORDY`, and dev.13's counters show 3,663 data reads held past 400 ns (worst 1,100 ns), every one waited for. So the guess below, that this controller doesn't sample IORDY on the data port, was wrong. The likelier cause is the BB48's wiring: long, undamped DIOR# leads (the PCB has 33 Ω series resistors on DIOR# and DIOW#), whose ringing could look like the strobe ending early to `iordy_check.pio`, or make the controller end it early. Not proven either way; the PCB doesn't show it. The analysis below is kept as it was.

On the Evesham with `bb48_hdcd` on `ide_bus` (dev.10), the host didn't wait for IORDY on 26 data reads during Doom (`NOIORDY ... last=DATA`), all while reading the CD and none while reading the hard disk; the CD-ROM build on early answers had 107 the same way (3 Nov). The game ran, so the words got there in time, but nothing promises that. What the logs and the code say (analysis only, no change yet):

- **They're all data reads, and only the CD's.** Over the session's 1,700-odd reads held past 1,250 ns, all were Status or Alternate Status (worst 3,590 ns), one LBA Mid and one Device: **no data read was ever held that long**, and the host waited for every status read. So this host waits on the command block registers but not, or not always, on the data port. PCI IDE controllers time the data port apart from the 8-bit registers (fast PIO timing, with IORDY sampled only as the BIOS set it per drive), which fits. Then any data read the drive answers late counts, however little late.
- **When:** each burst came right after a READ(10) of 1-3 frames (a seek in the WAD, not a stream). The `NOIORDY` lines come in pairs one second apart: the line's rate limit, so each burst was a few reads close together.
- **Not the IDENTIFY:** IDENTIFY PACKET DEVICE offers what the disk's IDENTIFY does (word 49 bit 11, IORDY supported; PIO mode 3 in word 64; words 67-68), so the BIOS has no reason to treat device 1 differently.
- **Not the fast lane, as such:** the CD's 512-byte DRQ blocks are served by core 1's fast lane like the disk's sectors. An ATAPI driver reads the byte count (LBA Mid, LBA High) between DRQ and the data, which takes core 1 out of the lane (`serveOther`), but the next pass of `loop1` goes straight back in (no bookkeeping step when an access was served), so that alone costs only a pass.
- **What differs for the CD:** small reads start with a PACKET whose first block comes from the fetch rather than the read-ahead, and each of a frame's four blocks raises INTRQ. If the driver's interrupt handler or its first `rep insw` comes as core 1 is busy with one of its steps (recording CMD/DONE, posting the fetch, taking a result, which run while the drive is BSY), the first data word waits that step out, up to a few microseconds, as the status reads do.

**What dev.13 measures without touching core 1:** each `NOIORDY` line now names the command the last unwaited read fell in or followed, its device, how long after it started and, if it had finished, how long after its DONE (`after=READ(10) d1 +1840us`, `done+120us`), and counts the data register's apart (`data=`); `s` adds `iordy_ignored_data`, and the data register's reads held past 400 ns in buckets (`iordy_data_slow=` up to 700 ns / 1,000 / 1,250 / longer) with their worst (`iordy_data_max_ns`). How to read them: if the unwaited reads come a fetch's time (about 1 ms or more) after the READ(10) started, they're the first block's first words, after the fetch; at a few hundred µs, within the read-ahead's blocks. If `iordy_data_slow` is nearly all in the first bucket, the host's data cycle is shorter than about 700 ns and any late word counts; `iordy_data_max_ns` gives the worst lateness. The times are when core 0 collected the report, within a loop pass of the read (up to about 0.5 ms behind it while it stores a sector), so they place a read within a command, not within a block.

**Then, if it isn't clear:** (1) `t` (raw bus trace) during a small CD read and look at the accesses around a `NOIORDY` (the trace slows core 1, so use it to see the order of accesses, not their timing); (2) the same Doom run with console `A` (read-ahead off): if the bursts grow, the first block's fetch is the late one; (3) `-DCMD_PROFILE=1` (bench checklist step 2b), whose `cmd_run_max_ns` says how long core 1 spends on a command while the host waits. **A candidate fix,** for after that: keep core 1 in the fast lane across the byte-count and status reads of a data-in block (serve them inside `serveDataIn` without returning), and run a PACKET's first fetch before its DRQ only from the lane. Both are core 1 changes, so they need `-m speed`, `-m timing` and a soak.

## The CD build's slow reads: what the code says (4 Nov 2026, 0.30.0-dev.6)

On the rig the CD-ROM build held Alternate Status reads up to 2.8-3.6 µs (the hard disk build, on the same early answers: about 1.3 µs at worst in DMA, 2.1 µs in a PIO speed run), and on the Evesham about 19,500 reads a session past 1,250 ns, worst 1,960 ns, where the hard disk build had none. Read from the code, without the bus:
- **Not the bus code's size:** the hot functions are a little bigger (`serviceOnce` 812 bytes against 768, `serveBusEarly` 1,424 against 1,348), tens of cycles at most; a 3.6 µs read is about 720 cycles.
- **Not flash:** no function core 1 reaches from `loop1` loads from flash. Its literal pools hold only the identity strings, read once at start-up, and the sense constants are immediates in `mmc::execute`. (`check_core1_ram.py` follows calls, not data loads, so this was checked by scanning the literal pools.)
- **The likely part:** in early answers' tight loop a command runs straight after the access that queued it, and for the CD-ROM that's the packet's last word: `runPacket` and `mmc::execute` (1,680 bytes, the biggest function on core 1), the trace's CMD and DONE records, then the device's next step, all before the host's next access, typically an Alternate Status poll, is answered. The hard disk's commands are short by comparison (IDENTIFY points at a buffer). Plus early answers' own latency, which the hard disk shows too.
- **The measurement:** a build with `-DCMD_PROFILE=1` prints `cmd_run_max_ns` and `cmd_run_max` (command/packet) in `s`: the longest time core 1 spent on a command while the host waited. If it's near the slow reads' excess over about 1 µs, the packet is the cause, and the fix is to answer the host's first poll (BSY) before running it, or to hand `mmc::execute` to core 0 as a fetch is. The first is built, off: `-DEARLY_POLL_FIRST_NS=1000` (0.30.0-dev.8, `ide_bus.cpp`'s `servePollFirst`); core 1 waits up to that long after a command is queued for the host's next access, answers it, then runs the command. If it's small, the cause is early answers itself, and the CD-ROM build should try `ide_bus` (`P i`), whose worst read on the hard disk is about 1.2 µs.

## Slow Status polls with a CD-ROM: core 1's register writes (11 Nov 2026, 0.32.2-dev.1-3, branch `cd-slow-status`)

On the Evesham (Doom's demo from the CD-ROM as device 1, `ide_bus`, PIO 3) 0.32.1 held 1.32 reads per READ(10) past ATA's 1,250 ns, 9,312 of 9,373 of them Alternate Status polls, median 1,780 ns, worst 4,270. They came 0.4-1.6 ms into each READ(10), while the drive showed BSY for the card read.

**Not interrupts:** core 1 has one interrupt enabled (`s` now prints `c1_irq`): IRQ 26, the SIO doorbell, which Arduino-Pico rings only to pause core 1 for a flash write (`idleOtherCore`), and whose handler is in RAM. A bench switch, `-DHDDPRO_C1_NO_IRQ=1`, masks it anyway.

**What the access log showed** (`-DHDDPRO_ACCESS_LOG=1`, its false alarm in the PIO data loop fixed): the worst polls came after the DOS driver's switch to the CD-ROM, Device Control (`08`) and Device (`b0`) written back to back and then Alternate Status read, with no empty FIFO between them. So the samples queued, and the poll waited behind both writes. With a CD-ROM each register write cost core 1 about 1.6 us: the two-device `write` about 0.7 us (whatever the register, even Device Control), the INTRQ update 0.33 us, and about 0.5 us either side for taking the sample, `serve`'s dispatch and the main loop. Reads cost about 1 us the same way.

**The changes**, all for `hdcd::Device` only (`if constexpr` in `serve`; the hard disk alone keeps its code, layout unchanged):
- `hdcd::Device::writeQuiet`: Device Control rewritten with the value it holds does nothing; Features, Sector Count and LBA go to both devices without `write()`'s command bookkeeping, and skip the INTRQ update; Device does the same but keeps it (the selection changes INTRQ).
- Reads other than Status (which clears the interrupt) and Data (which can end a block) skipped the INTRQ update in dev.2-3, as every other place core 1 changes a device's state seemed to update INTRQ itself (commands, fetch results, DMA bursts, the data-out and wake timers, RESET#). **Undone in dev.4:** on the USB adapter, which reads the CD-ROM by DMA, it slowed `-m cdrom` from 3.1 MB/s to 2.2-2.4 (`pcb_drive`, the same Mac and adapter). The drive's time per READ(10) stayed at 7.1 ms, but the gap from one DONE to the next command grew from 13 to 19 ms. With the skip built out (dev.3 with `-DHDDPRO_NO_QUIET_READS=1`) it read at 3.06 MB/s again. So something in the CD-ROM's DMA path still relies on a poll's INTRQ update; not found yet.
- INTRQ is worked out inline (`intrqInline`, both devices).
- A Device write that doesn't change the DEV bit still updates INTRQ: a device ignores writes while busy, so its own Device register can lag, and skipping could leave a host waiting for an interrupt.
- Tested: `hdcd_quiet_writes_match_write` drives two devices alike, one through `write()` only, and checks every register and INTRQ after each write (486 checks).

| On the Evesham | Slow per READ(10) | Worst |
|---|---|---|
| 0.32.1 (2 h) | 1.32 | 4,270 ns |
| dev.2 (quiet writes, reads) | 0.64 | 2,810 ns |
| dev.3 (and Device, inline INTRQ) | 0.49 | 2,420 ns |
| dev.4 (dev.3 without the read-side skip; `pcb_drive`, the card on SDIO) | 0.55 | 2,390 ns |

No sector wrong on the cable, `iordy_ignored=0`, no card lost, throughout.

**What's left:** a poll after a Device write: about 0.5 us of main loop either side of each access, plus the write (0.43 us) and INTRQ (0.26 us) with the access log on. Under 1,250 ns for every poll would need a fast path for register accesses ahead of the main loop. Hosts that wait for IORDY (all tried so far) only lose time.

## Slow Device reads on `ide_bus` (10 Nov 2026, 0.32.0-dev.5)

Since 0.30.0-dev.10 a CD-ROM always runs `ide_bus`, so the 4 Nov analysis above (early answers' tight loop running a packet straight after its last word) doesn't cover what the USB adapter showed on 10 Nov, at detection, with `device1 = cdrom` and DMA off (so with the diagnostics):

| Before | Slow read | Held |
|---|---|---|
| every IDENTIFY (device 0 and 1) and IDENTIFY PACKET | Device | 4,900-5,620 ns |
| straight after, the same commands | Alternate Status | 2,100-2,400 ns |
| SET FEATURES (device 1), TEST UNIT READY | Alternate Status | 3,300-3,400 ns |
| IDENTIFY PACKET's data | Status | 1,960-2,310 ns |

ATA allows 1,250 ns. The adapter waits for IORDY on all of them; a host that doesn't would read them wrong. Repeatable to within 15% over three detections, so systematic, not contention.

What the code says:
- **Not flash:** of the flash addresses in core 1's literal pools, only `mmc::execute`'s and `modeSense`'s sense codes are loaded, per command, not per access. `atapi::Device::deliverSector`'s is a sense code too.
- **Not the selection:** `ata::Device::deviceWritten` and `selectUnit` are a few loads and stores.
- **A heavier write:** before a command the adapter writes five registers back to back (Device, Sector Count, LBA low, mid, high: `host/reference/slice0_probe_jmicron.txt`), about 0.8 us apart, then reads Device. With a CD-ROM each write is `hdcd::Device::write`: both devices' `write`, `noteStarts`, then `updateIntrq` calling `intrq()` and `intrqDriven()` on the selected one, all out of line, perhaps 0.6 us where the hard disk alone takes about 0.35. That makes a backlog, but not one of 5 us.

**Measured (10 Nov, dev.5 and dev.6, CHANGELOG):** the access log never froze on these reads, so core 1 was idle, its FIFO empty, less than 1.25 us before answering each: not the register writes, and the two fixes below made no difference (dev.6 had them as build switches; dev.7 removed them). The slow Device reads show without a CD-ROM too (1.7 us at worst, at detection, slice 1). Next, at the desk: `ide_bus.pio`'s path from the strobe to its push, after a burst of register writes; and the access log's false alarm (core 1's PIO data loop doesn't note an empty FIFO).

How it was measured: `-DHDDPRO_ACCESS_LOG=1` (`src/access_log.h`) prints, for a read answered late, everything core 1 did in the microseconds before it (`ACCLOG` lines). `host/runs/bench/after_replug.sh` (git-ignored) captures three detections with it. Fixes to weigh once it's known: the plain task-file registers written only to the selected device and copied to the other at selection or command time; `updateIntrq` only when the interrupt can have changed; the device calls inlined.

## Status (3 Nov 2026, 0.29.0): the MVP is built, and reads on a retro PC

The firmware side of slices C1 and C2: the `bb48_cdrom` build serves the test disc or an ISO over PIO, and `test/run_native_tests.sh` drives `atapi::Device` through whole PACKET exchanges.

**First hardware (3 Nov, the Evesham, Athlon XP 2000+, PIO mode 3, early answers):** slice C0 was skipped, and C1 and C2 ran on a retro PC first, ahead of slice C3's plan. The USB adapter's runs (`-m cdrom`, 20 replugs) are still to do.
- **The BIOS** lists the drive as an ATAPI device (DEVICE RESET, IDENTIFY PACKET DEVICE, MODE SENSE(10) with the unit attention first) and tries to boot from it: READ(10), NOT READY with no disc. Five starts, the same each time.
- **A Windows 98 startup floppy** (`OAKCDROM.SYS` and MSCDEX) makes it D:. The driver also probes device 1 and correctly finds nothing; a second CD-ROM letter (E:) was the PC's own optical drive. It asks READ SUB-CHANNEL (42h), for the audio position, and takes ILLEGAL REQUEST.
- **The test ISO** (`HDDSYNTH.ISO`): `DIR D:` lists it and `CDCHECK` (on the disc, below) passes all 13 files, 216 READ(10) commands and none failed.
- **A real disc: `DOOM_SE.ISO`** (22 MB, ISO 9660, in six pieces on the card; `E cdrom = DOOM_SE.ISO`, `!`). The CD runs Doom without installing it: from the startup floppy, `MD C:\DOOMDATA` (the floppy's RAM drive is C: when the PC has no hard disk; Doom's `-cdrom` mode keeps its settings and saves there), then `D:`, `CD \DOOMCD`, `SETUP`, `DOOM`. It loaded and played: 2,474 READ(10), none failed, the same three expected refusals as before, nothing stuck. **107 data reads the host didn't wait for IORDY** (`iordy_ignored`, in a few bursts as the game loaded; 0 with the test disc). The hard disk build saw the same on this PC on 31 Oct (247), and early answers covered them; the game running says they did here too, but nothing checks a real disc's data on the cable (the bus observer knows only the generated disc).
- **Reads past 1,250 ns:** about 19,500 in the session, worst 1,960 ns (Alternate Status; mostly 1,600-2,000 ns, on every register, data included), `iordy_ignored=0`. The host waited for each one, but they're over ATA's limit. The hard disk build on early answers had none on this PC (the data lane had about 1,400, all Status), so something in the CD build costs core 1 more per access: `atapi::Device`'s inline paths (`peek`, `consumeRead`, the per-block fetch with no read-ahead) or where its code sits in RAM. To find first: `s` on the hard disk build against this one on the same PC, then the USB adapter's `-m cdrom` for a repeatable figure.
- **Through the USB adapter in DMA (4 Nov, 0.30.0-dev.3, `mode = mdma2`): it works.** PACKET's data now goes by multiword DMA when offered (below), and the adapter takes it: Windows 11 lists `HDDSYNTH PRO CD-ROM USB Device` as D:, `-m cdrom` passes (the 64 MB test disc read and checked byte for byte at 2.42 MB/s, random reads, 103,103 sectors checked on the cable, 0 bad), and with `DOOM_SE.ISO` Windows mounts the CDFS volume `DOOM_SE` and reads all 27 files (22.7 MB, 1.36 MB/s, no errors). The adapter picks MDMA 2 at detection (SET FEATURES 03h 22h) and polls GET EVENT STATUS NOTIFICATION once a second. DMA follows the hard disk's settings, so it's off by default: this adapter needs `mode = mdma2` (or `dma = on`) for the CD-ROM build. Worst read in the session 3,590 ns (Alternate Status), the slow reads below.
- **On the USB adapter (4 Nov, 0.30.0-dev.2 on branch `merge-cdrom`; the Windows rig's adapter, the one that shows as JMicron 152D:2338): it doesn't work yet, because the adapter wants DMA for packets.** Detection goes through (IDENTIFY DEVICE aborted with the packet signature, IDENTIFY PACKET DEVICE twice, SET FEATURES 03h 0Bh for PIO flow-control mode 3). Then it writes FEATURES = 01h, the DMA bit, with no byte count limit, and PACKET (for TEST UNIT READY), although IDENTIFY PACKET DEVICE offers no DMA (word 49 bit 8 clear). The drive aborts it at once (ERR, ABRT), as written. The adapter never retries in PIO: it polls Status for about 17 s, resets the drive, detects it again, and asks the same, every 20 s. So Windows shows no CD-ROM drive, and `-m cdrom` can't run on it. Not from the merge: the adapter decides before the drive has answered anything but IDENTIFY, and `atapi::Device` is atapi-cdrom's, unchanged but for one field. **Fixed in dev.3** by PACKET's data phase in multiword DMA (above). The Vantec adapter, when it arrives, is a second opinion.
  - Seen on the way: worst read 2,830 ns (Alternate Status) over these cycles, against 1,320 ns for the hard disk build on the same rig, so the CD build's slow reads (above) show here too, repeatably. And the trace's `DONE` for the aborted PACKET came only at the next reset, with a false `STUCK` line meanwhile: in early answers' tight loop, `DONE` is recorded when the bus goes quiet, and this adapter polls without a pause.

Where it differs from the plan below:
- **No templates.** `src/drive_device.h` makes `DriveDevice` an alias, `ata::Device` or `atapi::Device` per build, and `ide_bus` and `loop1` take a `DriveDevice&`. The hard disk build compiles to the same core 1 code as before, instruction for instruction at the same addresses (checked against 0.28.0's ELF). Only the calls into flash for one-offs (the linker's veneers, in a new order) and data addresses (0x40 bytes later, so every word stays in its SRAM bank) moved. `atapi::Device` has its own `CommandInfo` (with the packet's opcode) for the same reason: one more field in `ata::CommandInfo` lengthened `ata::Device::startCommand` by 4 bytes and moved everything after it.
- **No ISO means no disc**, not the test disc: the drive answers NOT READY, "medium not present", as a real one with an empty tray does. The test disc is `cdrom = none` or console `T`.
- **A few more commands** than the table: MODE SENSE(6), SEEK(10), VERIFY(10) and READ TOC's full TOC (format 2), cheap to answer and asked for by some hosts. GET EVENT STATUS NOTIFICATION reports "new media" once after power-on or a reset, then "no change". GET CONFIGURATION and GET EVENT STATUS NOTIFICATION report no unit attention, as MMC asks.
- **Status after a reset is 00h** (DRDY clear until a command has run), and IDENTIFY PACKET DEVICE offers accelerated DRQ (the packet is wanted within 50 µs, with no interrupt): the drive sets DRQ at once. Both are for slice C0's trace to confirm.
- **No read-ahead:** each 512-byte block is fetched under BSY once the host has read the one before. A 1x CD is 150 KB/s; this should manage several times that.

To try it: `PIO_ENV=bb48_cdrom scripts/rig.sh -m cdrom`, with `E cdrom = none` and `!` (or `T`) for the test disc. On Windows, set `HDDPRO_CDROM_DEV=\\.\CdRom0`.

### On the bench, step by step

**Slice C0: does the adapter pass ATAPI?** No emulator yet: a real IDE CD-ROM drive and the listen build.
1. A real IDE CD-ROM drive, jumpered master, on the adapter's 40-pin connector, powered from the adapter's brick.
2. Does the computer see it? macOS: System Information → USB, and `diskutil list` with a data CD in (does it mount?). Windows: Device Manager, and a drive letter for the disc. If not, stop here: the firmware can't help, and another adapter is the next step.
3. To record it: an IDE ribbon with two connectors, the adapter on one end, the drive and the BB48 (on the listen build, `PIO_ENV=bb48`, which never drives the bus) on the others. Console `w` (the capture window), then replug the adapter, put in a disc, open a file.
4. Save the serial log as `host/reference/c0_cdrom_<adapter>.txt` and list its exchanges: `cd host && .venv/bin/python -m hddproto.packets ../host/reference/c0_cdrom_<adapter>.txt`. Its last lines name any opcode the host used that `src/mmc.h` doesn't handle; those go into `mmc` before C1.

**On a retro PC with DOS** (as the Evesham ran it): a Windows 98 startup floppy, with CD-ROM support. `D:`, `DIR`, then `CDCHECK`, which the test ISO carries (`tools/cdcheck`, built by `test_iso` when NASM is installed): it reads every file whose ISO 9660 name DOS can open and checks its size and CRC-32, ending `all ok` or naming each bad file; `CDCHECK /R` repeats until a key, as a soak. The serial log goes on the Mac as usual. The drive looks for the ISO at start-up, so after putting the card back, `!`.

**Slices C1 and C2: the emulator.**
1. Flash and test in one go: `PIO_ENV=bb48_cdrom scripts/rig.sh -m cdrom`. Console `h` should say `READY HDDSynthPro 0.29.0 slice3 cdrom`, and `S` the `CONFIG cdrom=... serving=... frames=...` line.
2. **An empty drive first** (C1): `E cdrom = NOSUCH.ISO`, `!`. The host should list a CD-ROM drive from HDDSYNTH with no disc. The trace at detection: `CMD .. ec IDENTIFY` with `DONE .. ec 41 04` (aborted, as it should be), then `a1 IDENTIFY_PACKET`, then packets such as `a0 INQUIRY` and `a0 TEST_UNIT_READY` ending `DONE .. a0 41 20` (not ready).
3. **The test disc**: `E cdrom = none`, `!` (or console `T`), replug the adapter, then `-m cdrom`: `test_cdrom.py` reads the whole disc and at random. macOS says it can't read the disc: choose Ignore. Windows: `HDDPRO_CDROM_DEV=\\.\CdRom0`.
4. **The test ISO** (C2's exit): with the card in the computer, `cd host && .venv/bin/python -m hddproto.test_iso build /Volumes/<CARD>/HDDSYNTH.ISO` (it only adds that one file). Back in the drive, `E cdrom = HDDSYNTH.ISO`, `!`; `-m cdrom` then runs `test_cdrom_iso.py`, which checks every file on the mounted disc.
5. **If detection fails:** `t` (the raw trace), replug, save the log and run `hddproto.packets` on it, as for C0; `s` for `iordy_max_ns` and `iordy_ignored`. Compare with the C0 trace: the first exchange that differs is the place to look.
6. Back to the hard disk: flash `bb48_drive` again. The `cdrom` line in `HDDSYNTH.INI` can stay; the hard disk ignores it.

**Beside the other explorations** (not merged, and kept apart on purpose): the two-drives idea (`docs/two-drives.md` on its own branch: a pair of devices behind one bus-facing object) could hold an `atapi::Device` as device 1 the same way it would a second `ata::Device`, since both have the same interface; that's where a hard disk and a CD on one cable would come from. A read-only disk built from a folder on the card could in time feed a disc too, with ISO 9660 in place of FAT. Neither is part of this MVP.

## Goal and scope

Find out what it takes for the drive to be an IDE CD-ROM drive serving a data ISO from the SD card. Do it in thin slices, as [plan.md](plan.md) did for the hard disk, without touching the hard disk's proven path.

**In scope**
- One device, a CD-ROM drive, as device 0, alone on the cable, in a build of its own (`bb48_cdrom`).
- ATAPI over PIO: the PACKET command, with the host's 12-byte command packets.
- A data disc: one Mode 1 track of 2,048-byte sectors, from an `.ISO` file on the card, or a generated test disc.
- The smallest set of SCSI multimedia (MMC) commands that lets a host find the disc, read its table of contents and read its data.
- Read-only. The disc is chosen at boot, with no disc changes while running.

**Out of scope for now**
- Audio: CD audio tracks, BIN/CUE and other raw 2,352-byte images, PLAY AUDIO, READ CD (raw sectors), CD-Text, subchannel data.
- Mode 2 and XA sectors, multisession discs, DVDs, writing.
- DMA, and PIO faster than what the hard disk build offers.
- A hard disk and a CD together (devices 0 and 1), and swapping discs while running.
- The Amiga: its built-in driver needs Kickstart 3.1.4 or later for ATAPI.

**Done means:** through the USB-IDE adapter, macOS mounts an ISO from the SD card and every file reads back identical to the source. On the Pentium 200, Windows 98 reads the same disc with its own driver.

## Why a build of its own

The hard disk build's core 1 timings are tens of nanoseconds from ATA's limits, and moving its code in RAM has moved them before (CLAUDE.md). An exploration shouldn't risk that. A separate PlatformIO environment, `bb48_cdrom`, leaves the `bb48_drive` binary exactly as it is.

Merging the two into one binary is a decision for later, once the CD-ROM works. That later step is where a hard disk and a CD on one cable would come in, and it touches the [one-build reasoning in pio-allocation.md](pio-allocation.md#why-c).

## Firmware shape

What carries over unchanged:
- **The bus:** `ide_bus.pio`, core 1's register path and its IORDY timing, and the diagnostics (`iordy_timer`, `bus_observer` for IDENTIFY-style checks). A CD-ROM drive uses the same registers, strobes and 16-bit data words.
- **The data path:** the fetch mailbox, `BlockSource` and `ImageDisk` over a file on the card, and read-ahead. A CD sector is four of today's 512-byte sectors, so an ISO is an image read in steps of four: CD sector *n* is image sectors 4*n* to 4*n*+3.
- **The settings file, the trace and the console.**

What's new:
- **`src/atapi_device.*`: `atapi::Device`,** the ATAPI register file and state machine, pure C++ and host-tested like `ata::Device`. It reuses `ata::Reg`, the status bits, `Fetch` and `CommandInfo`. It offers the interface core 1's loop and `ide_bus.cpp` use today (`peek`/`consumeRead`, `write`, `dataInRemaining`/`consumeDataWords`, `pendingFetch`/`fetchDone`, `intrq`, `hardReset`, the command counters), so the same loop can drive it.
- **`ide_bus` and `loop1()` become templates over the device type,** instantiated once per build. The hard disk build's instantiation must compile to the same code as now: check `check_core1_ram.py`, and its worst read on the rig, before and after.
- **`src/mmc.*`: the packet commands,** pure C++: a 12-byte packet in; out comes a reply (data to send, or none), a status, and sense data for errors. Kept apart from the bus state machine so each command is unit-tested on its own.
- **A `cdrom` key in `HDDSYNTH.INI`:** the ISO to serve (default `HDDSYNTH.ISO`), or `none` for the generated test disc. The hard disk build ignores it, so one card can carry both builds' settings.

### The ATAPI protocol, in brief

From ATA/ATAPI-5 (T13 1321D) for the register protocol and MMC-2 for the commands. Check every detail against the spec and the reference trace from slice C0.

- **Signature:** after a hardware reset, a soft reset, DEVICE RESET (08h) or EXECUTE DEVICE DIAGNOSTIC, the registers read Sector Count 01h, LBA Low 01h, LBA Mid 14h, LBA High EBh. That's how a host knows it has found a packet device.
- **IDENTIFY DEVICE (ECh) is aborted,** with the signature left in the registers. **IDENTIFY PACKET DEVICE (A1h)** returns the device's 256 words: word 0 says ATAPI, CD-ROM (type 05h), removable, 12-byte packets. The model is `HDDSYNTH PRO CD-ROM`, with the PIO modes and IORDY as the hard disk offers them.
- **PACKET (A0h):** the host sets Features (bit 0 clear: PIO) and the most bytes it takes per DRQ block (LBA Mid/High), then writes A0h.
  1. The device asks for the packet: DRQ, with the interrupt reason (Sector Count) set to "command, out" (CoD = 1, IO = 0). The host writes 6 words.
  2. BSY while the device acts on it.
  3. Data in, per DRQ block: the block's length in LBA Mid/High, the interrupt reason "data, in" (CoD = 0, IO = 1), DRQ and an interrupt. The host reads that many bytes.
  4. The end: the interrupt reason "status" (CoD = 1, IO = 1), DRQ clear, an interrupt. On an error, ERR in Status ("CHECK CONDITION") and the sense key in the top four bits of the Error register.
- **DRQ blocks of 512 bytes:** one fetched sector each, as now. Legal, since the device chooses each block's length up to the host's limit, and it keeps the buffer, the mailbox, read-ahead and core 1's fast lane as they are. A 2,048-byte frame is four blocks. Some old DOS drivers may assume one block per frame: slice C3 finds out (see the open questions).
- **The housekeeping commands** the hard disk accepts (SET FEATURES transfer mode, CHECK POWER MODE, IDLE, STANDBY and so on) behave the same. Anything else gets ABRT.

### The commands for a data disc

| Opcode | Command | The minimal answer |
|---|---|---|
| 00h | TEST UNIT READY | Good with a disc; NOT READY, "medium not present" (02h/3Ah) without one |
| 03h | REQUEST SENSE | The last error's sense data (fixed format, 18 bytes) |
| 12h | INQUIRY | Device type 05h, removable, vendor `HDDSYNTH`, product `PRO CD-ROM`, the firmware version |
| 1Bh | START STOP UNIT | Accepted; eject and load do nothing yet |
| 1Eh | PREVENT ALLOW MEDIUM REMOVAL | Accepted |
| 25h | READ CAPACITY | The last sector's address, and 2,048 |
| 28h, A8h | READ(10), READ(12) | The sectors, from the ISO or the test disc; past the end: ILLEGAL REQUEST, "LBA out of range" (05h/21h) |
| 43h | READ TOC | Format 0: one data track (track 1 from sector 0) and the lead-out; format 1: one session |
| 46h | GET CONFIGURATION | The CD-ROM profile (0008h) and the few features it implies |
| 4Ah | GET EVENT STATUS NOTIFICATION | Polled: "no change"; with a disc, "media present" |
| 5Ah | MODE SENSE(10) | The pages hosts ask for (error recovery 01h, CD capabilities 2Ah), fixed values |
| BBh | SET CD SPEED | Accepted |
| others | | CHECK CONDITION, ILLEGAL REQUEST, "invalid command operation code" (05h/20h) |

The first command after power-on or a reset gets UNIT ATTENTION, "power on or reset" (06h/29h), as a real drive does. INQUIRY and REQUEST SENSE don't. Each host probes a slightly different set of commands, so expect to add to this table from traces, as happened with the hard disk's housekeeping commands.

### The generated test disc

`cdrom = none`, or no ISO on the card, serves a test disc: CD sector *n* is `gen_disk` sectors 4*n* to 4*n*+3, 64 MB in all. It has no file system, so nothing mounts it, but every byte read can be checked, on the host against `gen_disk.py` and on the cable by the bus observer, as with the hard disk's test disk.

## Thin slices

Each slice ends in a passing automated test before the next starts.

- **Slice C0: does the adapter do ATAPI?** No firmware needed.
  - Plug a real IDE CD-ROM drive into the NS1081. Does macOS show it and mount a disc? Most USB-IDE bridges pass ATAPI through, but this one is unproven.
  - Record the adapter talking to the real drive with the listen build: the BB48 on a second connector of an IDE ribbon (it never drives the bus), or on the adapter's 44-pin header, which shares the bus with the 40-pin one.
  - **Exit:** a disc mounts through the adapter, and a reference trace of its detection and mount is saved in `host/reference/` (the reset signature, IDENTIFY PACKET DEVICE, the packets in order).
- **Slice C1: a CD-ROM drive with no disc.**
  - The `bb48_cdrom` build: the signature, IDENTIFY DEVICE aborted, IDENTIFY PACKET DEVICE, DEVICE RESET, the PACKET protocol, the housekeeping commands. Every packet command answers NOT READY, "medium not present", except INQUIRY and REQUEST SENSE.
  - Host unit tests drive `atapi::Device` through whole PACKET exchanges, register by register, as the adapter does in the reference trace.
  - **Exit:** macOS lists an empty CD-ROM drive from `HDDSYNTH` on 20 replugs out of 20, with IORDY within 1,250 ns throughout.
- **Slice C2: read a data disc.**
  - The table above, first on the generated test disc, then on an ISO from the card.
  - **Exit:** a full read of the test disc and random reads with no mismatches, on the host and on the cable. An ISO made with `hdiutil makehybrid -iso -joliet` mounts in Finder, and every file's checksum matches its source.
- **Slice C3: a retro host.**
  - Windows 98 on the Pentium 200 (driver built in), then DOS with `OAKCDROM.SYS` and `MSCDEX`.
  - **Exit:** both copy the ISO's files off with matching checksums. If the DOS driver fails on 512-byte DRQ blocks, this is where 2,048-byte blocks get built.

## Rig tests

A new pytest marker, `cdrom`, with tests that skip unless the board runs the `bb48_cdrom` build. As in the hard disk's tests:
- `find_disk` finds the drive by name: the adapter reports INQUIRY's vendor and product for a packet device, rather than splitting an ATA model.
- Reads go through `/dev/rdiskN` on macOS, in whole 2,048-byte sectors, and `\\.\CdRomN` on Windows.
- Disc images for the tests are made at test time with `hdiutil makehybrid`, from a folder of generated files, so nothing large is checked in.

## Risks and open questions

| Risk | Mitigation |
|---|---|
| The NS1081 doesn't pass ATAPI | Slice C0 finds out before any firmware; a second adapter with another chipset. **The Windows rig's adapter (JMicron ID) passes it, but asks for DMA for every packet** (4 Nov): PACKET in multiword DMA (dev.3) serves it, with `mode = mdma2` |
| macOS asks for more than the table | The C0 reference trace lists what it asks; answer from that |
| 512-byte DRQ blocks break an old driver | Slice C3; 2,048-byte blocks need a 2 KB buffer, four fetches before DRQ, and a larger read-ahead |
| The host's byte count limit under 512 | Rare; answer with blocks of the limit, rounded down to even, or CHECK CONDITION at first |
| Making the bus layer a template moves the hard disk build's core 1 code | Compare `check_core1_ram.py` and the slice 1 worst read before and after; keep the change apart from any CD-ROM work |
| Status after reset (DRDY clear for packet devices?) | Follow ATA/ATAPI-5 and the C0 trace; a host that sees the wrong status won't find the drive |

**Questions for after this exploration:**
- One binary with both a hard disk and a CD-ROM (devices 0 and 1), or a CD-ROM mode chosen in `HDDSYNTH.INI`? **Both, in `bb48_hdcd`** (above): device 1 a CD-ROM from `HDDSYNTH.INI`. A CD-ROM as device 0 alone stays `bb48_cdrom`'s. **Since 0.32.0 one build does all of it** (top).
- Swapping discs at run time from the console or the HDD Synth (UNIT ATTENTION, "medium may have changed", 06h/28h, and GET EVENT STATUS NOTIFICATION's media events). **From the console since 0.30.0-dev.7** (above); the HDD Synth to come.
- Audio tracks: BIN/CUE images, PLAY AUDIO through the HDD Synth's speaker, READ CD for raw sectors.
- The Amiga, which needs Kickstart 3.1.4 or later, or a third-party ATAPI driver, and a CD file system.

## A future idea: Iomega ZIP and LS-120

Beyond the CD-ROM, and only once it works. Noted 28 Oct 2026.

The IDE versions of the Iomega ZIP 100 and the LS-120 superdisk are removable ATAPI drives too, so this exploration's work carries over: the reset signature, IDENTIFY PACKET DEVICE, the PACKET protocol, sense data and UNIT ATTENTION.

What they add:
- **Direct-access devices** (device type 00h, not 05h), with 512-byte sectors, so the hard disk's sector path fits them unchanged.
- **Writes:** WRITE(10), with VERIFY and MODE SENSE's write-protect bit. These are the first writable packet devices, so the image on the card changes, as with the hard disk.
- **READ FORMAT CAPACITIES** (23h), which hosts use to size removable media, and the eject and write-protect commands.
- **Iomega's vendor commands**, which its own tools (IomegaWare, ZIP Tools) use for disk status and protection modes. Find them from a trace of a real drive, as in slice C0.
- **The images:** a ZIP 100 disk is 196,608 sectors (96 MB). The LS-120 also reads 1.44 MB floppies, so it could serve floppy images (`.IMA`) to a host whose BIOS boots from an LS-120.
- **Changing disks** is the point of these drives, so it can't wait as it does for the CD-ROM. Eject and insert come from the console, the settings file or the HDD Synth, reported to the host as "medium may have changed".

**On the HDD Synth:** the activity link could tell the synth which drive it's emulating, so it plays a ZIP drive's whir, clicks and eject in place of a hard disk's sounds. The link's version 1 already lets a later version add fields at the end ([activity-link.md](activity-link.md)): a device type field (hard disk, CD-ROM, ZIP, LS-120), plus insert and eject events, would fit. The synth needs a sound set per drive.
