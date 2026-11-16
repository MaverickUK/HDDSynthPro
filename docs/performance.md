# Performance roadmap: faster reads and writes on rev 0.3

**Status (15 Nov 2026): a plan, from a review at the desk. On branch `performance`: item 1 is written and on the Evesham (0.33.1-dev.9, 16 Nov): reads 34% faster on early answers, but 15% slower on the data lane, to be fixed before `main`; item 2 is a test plan needing no code.** Measurements go in [speed-log.md](speed-log.md) as they are made.

Rev 0.4 doesn't make the bus faster: it adds the PSRAM, which makes large caches possible. Everything below except the large form of items 4 and 5 can be done and measured on rev 0.3.

## Where rev 0.3 stands

| Path | Measured | Limit | What sets the limit |
|---|---|---|---|
| PIO 3 on the Evesham, the data lane | 4.24 MB/s reads, 2.03 writes | about 11 MB/s | the mode's 180 ns cycle |
| PIO 3 on the Evesham, early answers | 2.29 reads, 1.25 writes | the same | |
| MDMA 2 through the JMicron adapter, from RAM | 7.7 reads, 13.9 writes | 16.7 MB/s | the mode's 120 ns cycle |
| The card over SDIO at 25 MHz | 6.2-6.6 MB/s | about 12 MB/s at 50 MHz | the card's clock |
| PIO through the USB adapters | 1.2-2.65 MB/s | the adapter's pace | not ours to change |

PIO on a real PC runs at well under half of what its mode allows, and the card is the ceiling for anything served from an image. The USB adapters set their own pace, so the retro PCs are where these gains show: measure with `DOSBENCH.IMG` ([tools/bench-image](../tools/bench-image/README.md)) on the Evesham, before and after, on the same bus program.

## The items, biggest expected gain first

### 1. READ MULTIPLE, WRITE MULTIPLE and SET MULTIPLE MODE (0.33.1-dev.9: a win on early answers, a loss on the data lane)

**Why.** The drive refuses SET MULTIPLE MODE (C6h), so a DOS or Windows 9x host takes an interrupt, a Status read and a wait for DRQ for every sector. With multiple mode the host does that once per block of sectors. Not for the USB adapters: both ignore multiple mode.

**What needs doing.**
- `ata_device`: a DRQ block of up to `MAX_MULTIPLE` sectors instead of one. The two sector buffers become two block buffers (one being read by the host, one being filled, as read-ahead and write-behind use them now). `Fetch` gains a sector count, so core 0 moves a block per request; `FetchResult` says which sector failed, for the task file's LBA.
- SET MULTIPLE MODE: a power of two up to `MAX_MULTIPLE` sets it, 0 turns it off, anything else is aborted. Per unit; off after power-on and RESET#, kept over SRST. READ and WRITE MULTIPLE (C4h, C5h) are aborted while it is off.
- IDENTIFY: word 47 (the largest block), word 59 (the current one).
- Core 0 (`serveFetches`): a loop over the block's sectors; seek timing and the bus observer still see single sectors.
- Every bus program takes its block length from the device already (IDENTIFY's 256 words, a sector's): check each with a longer block (`ide_bus`'s fast lanes, early answers, the data lane's DMA count, the register table's stream).
- The trace's names, the activity classifier (C4h is a read, C5h a write), `host/hddproto`.
- **RAM:** two buffers of `MAX_MULTIPLE` sectors. At 8 sectors that is 7 KB more than today, which the 9.7 KB freed in 0.33.1-dev.3 pays for; 16 (what most BIOSes ask for if offered) needs 15 KB and waits for the RAM overlay's 264 KB (item 4).

**How to prove it.** Native tests for the device. On the Evesham: `DOSBENCH.IMG` before and after, on the data lane and on early answers; the BIOS's "block mode" setting on; `CMD c6` in the trace with the count it asked for, then `c4`/`c5`. Slices 3-4 and a soak through the adapter show nothing else changed (it never sends them).

**Risk.** It changes the device core 1 runs, the most-soaked code in the tree, and moves core 1's layout: benchmark and record it.

**On the rig (0.33.1-dev.9, 16 Nov).** Slices 3-4 pass on every bus program through the JMicron (which never sends the new commands). dev.8 slowed PIO writes 6-9%, because the block buffers sat ahead of the device's other members; dev.9 puts them last and is back to 0.33.0's speeds ([speed-log.md](speed-log.md)). Still owed: the Evesham with block mode on.

**On the Evesham (16 Nov, PCB, `pcb_drive`, PIO 3, `DOSBENCH.IMG` in 18 extents, no CD-ROM).** The AMIBIOS has no block-mode setting: it turns block mode on by itself whenever IDENTIFY offers it, sending SET MULTIPLE MODE with 8 after every reset, then only READ and WRITE MULTIPLE. So "before" is 0.33.0 (which doesn't offer it) and "after" dev.9, on the same disk and bus program; every run passed, 0 bad, no BIOS errors. Logs `host/runs/retro-20261116-*-evesham-*`.

| Bus program | Firmware | Seq read | Random read | Seq write | 63-sector read in the drive | 63-sector write |
|---|---|---|---|---|---|---|
| Early answers | 0.33.0 | 1.72 MB/s | 0.87 ms | 1.23 MB/s | 18.4 ms | 25.3 ms |
| Early answers | dev.9, blocks of 8 | **2.31** (+34%) | 0.87 | **1.33** (+8%) | 13.6 | 23.5 |
| Data lane | 0.33.0 | 3.96 | 0.76 | 2.03 | 7.75 | 15.2 |
| Data lane | dev.9, blocks of 8 | **3.35** (-15%) | 0.87 | 2.18 (+7%) | 10.3 | 14.3 |

- **Early answers:** the win the item was for. The per-sector Status poll and interrupt it removes were most of a read's cost there.
- **The data lane: reads 15% slower**, and single-sector reads 37 µs slower (603 against 566 µs). The device and the lane both ask for the next block's read-ahead as the host starts on the current one (`deliverSector`, `serveLaneT`), so the overlap is meant to be there; something in how the lane streams a 4,096-word block, or in core 0 filling one, loses it. Not yet known. Until it is, the drive shouldn't offer block mode while the lane runs.
- **One `BUSERR` on the lane with blocks** (LBA 811, at boot: word 0 seen 0000, 211 words wrong), with `bus_lost` 56,858 against 2,128 on the lane without blocks (9 Nov). HDDBENCH's checks and the boot were right, so it's probably the observer losing its place in 8-sector blocks (`EXPECT_SLOTS`), not data on the cable; to be checked.
- Slow Status reads as before (the lane: 3,984 past 1,250 ns, up to 1,810, all waited for; early answers' worst 1,040 ns), `iordy_ignored` 0.

**Owed before `main`:** ~~block mode off on the data lane~~ (0.33.1-dev.10: not offered there; finding why the lane is slower with blocks is still open), ~~the `BUSERR`~~ (dev.11: the observer re-aligning on image data that looked like a generated sector's header, not the data), ~~a soak through the adapter~~ (dev.10, an hour on early answers, clean), and ~~the Evesham with the CD-ROM beside the disk (`ide_bus`)~~ (dev.11, 16 Nov: clean, and the data lane back to 0.33.0's speed with no block mode; CHANGELOG). Ready for `main`. On the way, dev.10 moved core 0's per-sector code into RAM: flash-only changes had been slowing the data lane's reads 10% ([speed-log.md](speed-log.md)).

**As built (0.33.1-dev.8).** `MAX_MULTIPLE` is 8 (identify.h): 8.8 KB more RAM, 96.9% used. Going to 16 needs 16 KB more, and the bus observer's `EXPECT_SLOTS` (16: a block with the host and one read ahead) doubled with it. Core 0 still makes one card call per sector inside the block (item 4 makes it one per block). In table mode a block of 8 is 2,048 data writes against the write ring's 1,024 samples, drained as it fills.

### 2. Multiword DMA on a real PC (started: a test plan, no code)

**Why.** MDMA 2 is soaked through the adapter (0.17.1) but has never run on a retro PC, and `dma` is off by default. On the Evesham it should lift reads from the data lane's 4.24 MB/s towards the card's 6.2-6.6, with no per-word work for core 1.

**What needs doing.** No firmware change to try it:
1. `E mode = mdma2`, `E dma = on`, `!`. `S` shows the mode offered; on the PCB with SDIO the diagnostics are off while DMA is offered (`pio_plan`), so `iordy_*` and `bus_*` read 0.
2. The Evesham's BIOS: the drive's transfer mode on auto (or "DMA"). DOS needs a DMA driver to use it at all: the BIOS's INT 13h is PIO on most boards. Windows 98 uses it with the DMA box ticked in Device Manager. So the DOS benchmark may show no change while Windows does: note which was measured.
3. Look for `CMD ef` with count `22h` (set MDMA 2), then `c8`/`ca` in the trace, and `DONE` with no error.
4. `DOSBENCH.IMG` if DOS uses DMA there; else a file copy timed in Windows 98, both ways, against the same in PIO.
5. With a CD-ROM as device 1: Doom from the disc again, since PACKET's data goes by DMA too when offered.

**Then.** If it's clean on the Evesham and the Pentium 200: `DEFAULT_DMA` on. A host that won't use DMA just stays in PIO.

### 3. The card at 50 MHz (SD high-speed mode)

**Why.** The card is the ceiling for an image: 6.2-6.6 MB/s at 25 MHz. High-speed mode doubles the clock.

**What needs doing.** CMD6 (switch function) after the card is up over SDIO, check its answer, then the PIO clock divider for 50 MHz; fall back to 25 MHz if CMD6 fails or the first reads show CRC errors. `sdio_card.pio`'s sampling point may need moving at 50 MHz.

**Risk.** Signal quality on the PCB's SDIO traces: a 10-hour card soak, as 0.31.0 had, on more than one card.

### 4. Runs of sectors to and from the card

**Why.** Core 0 reads one sector ahead and stores one sector at a time, each a card command. A card streams several sectors per command far faster than it starts commands.

**What needs doing.** Item 1 gives `Fetch` a sector count; `ImageDisk` then reads or writes the run in one call (the card layer's `readSectors`/`writeSectors` already take a count). Beyond that: read-ahead of more than one block, and writes gathered into runs before they are stored, both of which need buffer RAM. **The RAM overlay** holds 264 KB whether it is used or not (only the welcome disk and the test disk write through it): freeing it when an image is served pays for this and for item 5. Check first what slices 3-4 and the soak need of it (they write to the test disk).

### 5. A read cache

**Why.** Repeated reads: the FAT and directories, which DOS reads again and again. It doesn't help sequential speed.

**What needs doing.** A small cache of sectors by LBA on core 0, in the RAM item 4 frees; writes update it. On rev 0.4 the PSRAM holds a large one, with write-back.

### 6. PIO writes

**Why.** On the Evesham writes run at half the reads' speed (2.03 against 4.24 MB/s on the data lane).

**What needs doing.** Measure first: how much is the 10 µs BSY after each sector (`DATA_OUT_BSY_US`), how much the single store in flight, how much the card. Item 1 removes the per-sector part for hosts that use WRITE MULTIPLE.

## What needs rev 0.4

Megabytes of cache and write-back in the PSRAM, in place of the few hundred kilobytes items 4 and 5 can find on rev 0.3. Ultra DMA needs no new hardware, but isn't worth doing while the card tops out near 6 MB/s.
