# Performance roadmap: faster reads and writes on rev 0.3

**Status (15 Nov 2026): a plan, from a review at the desk. On branch `performance`: item 1 is written and host-tested (0.33.0-dev.9), item 2 is a test plan needing no code; nothing here is measured yet.** Measurements go in [speed-log.md](speed-log.md) as they are made.

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

### 1. READ MULTIPLE, WRITE MULTIPLE and SET MULTIPLE MODE (written in 0.33.0-dev.9; not on hardware)

**Why.** The drive refuses SET MULTIPLE MODE (C6h), so a DOS or Windows 9x host takes an interrupt, a Status read and a wait for DRQ for every sector. With multiple mode the host does that once per block of sectors. Not for the USB adapters: both ignore multiple mode.

**What needs doing.**
- `ata_device`: a DRQ block of up to `MAX_MULTIPLE` sectors instead of one. The two sector buffers become two block buffers (one being read by the host, one being filled, as read-ahead and write-behind use them now). `Fetch` gains a sector count, so core 0 moves a block per request; `FetchResult` says which sector failed, for the task file's LBA.
- SET MULTIPLE MODE: a power of two up to `MAX_MULTIPLE` sets it, 0 turns it off, anything else is aborted. Per unit; off after power-on and RESET#, kept over SRST. READ and WRITE MULTIPLE (C4h, C5h) are aborted while it is off.
- IDENTIFY: word 47 (the largest block), word 59 (the current one).
- Core 0 (`serveFetches`): a loop over the block's sectors; seek timing and the bus observer still see single sectors.
- Every bus program takes its block length from the device already (IDENTIFY's 256 words, a sector's): check each with a longer block (`ide_bus`'s fast lanes, early answers, the data lane's DMA count, the register table's stream).
- The trace's names, the activity classifier (C4h is a read, C5h a write), `host/hddproto`.
- **RAM:** two buffers of `MAX_MULTIPLE` sectors. At 8 sectors that is 7 KB more than today, which the 9.7 KB freed in 0.33.0-dev.4 pays for; 16 (what most BIOSes ask for if offered) needs 15 KB and waits for the RAM overlay's 264 KB (item 4).

**How to prove it.** Native tests for the device. On the Evesham: `DOSBENCH.IMG` before and after, on the data lane and on early answers; the BIOS's "block mode" setting on; `CMD c6` in the trace with the count it asked for, then `c4`/`c5`. Slices 3-4 and a soak through the adapter show nothing else changed (it never sends them).

**Risk.** It changes the device core 1 runs, the most-soaked code in the tree, and moves core 1's layout: benchmark and record it.

**As built (0.33.0-dev.9).** `MAX_MULTIPLE` is 8 (identify.h): 8.8 KB more RAM, 96.9% used. Going to 16 needs 16 KB more, and the bus observer's `EXPECT_SLOTS` (16: a block with the host and one read ahead) doubled with it. Core 0 still makes one card call per sector inside the block (item 4 makes it one per block). In table mode a block of 8 is 2,048 data writes against the write ring's 1,024 samples, drained as it fills.

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
