# ATAPI CD-ROM: plan for a first exploration

28 Oct 2026, planned. Branch `atapi-cdrom`.

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
- One binary with both a hard disk and a CD-ROM (devices 0 and 1), or a CD-ROM mode chosen in `HDDSYNTH.INI`?
- Swapping discs at run time from the console or the HDD Synth (UNIT ATTENTION, "medium may have changed", 06h/28h, and GET EVENT STATUS NOTIFICATION's media events).
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
