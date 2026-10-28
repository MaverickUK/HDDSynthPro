# ATAPI CD-ROM: plan for a first exploration

28 Oct 2026. A plan, nothing built yet. Branch `atapi-cdrom`.

## Goal and scope

Find out what it takes for the drive to be an IDE CD-ROM drive serving a data ISO from the SD card. Do it in thin slices, as [plan.md](plan.md) did for the hard disk, without touching the hard disk's proven path.

**In scope**
- One device, a CD-ROM drive, as master, alone on the cable, in a build of its own (`bb48_cdrom`).
- ATAPI over PIO: the PACKET command, with the host's 12-byte command packets.
- A data disc: one Mode 1 track of 2,048-byte sectors, from an `.ISO` file on the card, or a generated test disc.
- The smallest set of SCSI multimedia (MMC) commands that lets a host find the disc, read its table of contents and read its data.
- Read-only. The disc is chosen at boot, with no disc changes while running.

**Out of scope for now**
- Audio: CD audio tracks, BIN/CUE and other raw 2,352-byte images, PLAY AUDIO, READ CD (raw sectors), CD-Text, subchannel data.
- Mode 2 and XA sectors, multisession discs, DVDs, writing.
- DMA, and PIO faster than what the hard disk build offers.
- A hard disk and a CD together (master and slave), and swapping discs while running.
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
| The NS1081 doesn't pass ATAPI | Slice C0 finds out before any firmware; a second adapter with another chipset |
| macOS asks for more than the table | The C0 reference trace lists what it asks; answer from that |
| 512-byte DRQ blocks break an old driver | Slice C3; 2,048-byte blocks need a 2 KB buffer, four fetches before DRQ, and a larger read-ahead |
| The host's byte count limit under 512 | Rare; answer with blocks of the limit, rounded down to even, or CHECK CONDITION at first |
| Making the bus layer a template moves the hard disk build's core 1 code | Compare `check_core1_ram.py` and the slice 1 worst read before and after; keep the change apart from any CD-ROM work |
| Status after reset (DRDY clear for packet devices?) | Follow ATA/ATAPI-5 and the C0 trace; a host that sees the wrong status won't find the drive |

**Questions for after this exploration:**
- One binary with both a hard disk and a CD-ROM (master and slave), or a CD-ROM mode chosen in `HDDSYNTH.INI`?
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
