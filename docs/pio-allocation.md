# PIO block allocation (design, not yet built)

How the drive should share the RP2350's three PIO blocks once 4-bit SDIO for the microSD card joins the bus program, multiword DMA and the diagnostics. Decided 28 Oct 2026 (option C below); to be built when SDIO is taken on.

**On hold until DMA runs on hardware** (DMARQ and DMACK# wired; see [dma.md](dma.md)). SDIO speeds up reads only once DMA lifts the adapter's PIO pace, and the configuration that costs the diagnostics (an image with DMA) only exists then. Reviewed 28 Oct against SdFat's driver: see [Before starting](#before-starting) and the answered questions at the end.

## The problem

The RP2350 has three PIO blocks, each with 32 instruction slots and 4 state machines. Each block reaches one 32-pin window of GPIOs at a time: base 0 (GPIO 0-31) or base 16 (GPIO 16-47).

| User | Instructions | State machines | Window | Today (0.16.0) |
|---|---|---|---|---|
| `ide_bus.pio`: every register access, IORDY | 32 | 1 | base 0 (GPIO 4-30) | block of its own, always |
| `iordy_timer.pio` + `bus_observer.pio`: the diagnostics | 16 + 8 | 2 | base 0 (GPIO 4-30) | share a block |
| `ide_dma.pio`: multiword DMA's data phase ([dma.md](dma.md)) | 32 | 1 | base 0 (GPIO 4-31) | block of its own, always |
| SdFat's PIO SDIO driver (`SdCard/PioSdio`) | 31 | all 4 (uses 2) | base 16 (the card is on GPIO 34-39) | not used: the card runs over SPI0 |

Four users need four blocks; there are three. SDIO can't share a block with anything else: it needs a whole block (it claims all four state machines) and the base-16 window, while everything else needs base 0. The PCB keeps the same GPIOs, so it has the same limit.

Why SDIO at all: over SPI at 25 MHz the card delivers about 1.8 MB/s (about 280 µs a sector) and writes 0.94 MB/s (`speed-log.md`). Card writes are card-limited today; reads will be once DMA lifts the USB adapter's PIO pace. 4-bit SDIO at the same 25 MHz is about four times SPI's raw rate, but that speeds up only the transfer itself: a store takes about 550 µs a sector, of which moving 512 bytes over SPI at 25 MHz is about 165 µs. SDIO cuts that to about 40 µs, so expect writes of about 1.2 MB/s rather than the adapter's limit, unless the rest of the 550 µs turns out to be SPI overhead too (time it first); 50 MHz (high speed) should wait for the PCB, since 37.5 MHz over SPI failed on the rig's hand-wired leads.

## Tried on the rig (29 Oct 2026): not reliable on the hand-wired leads

A quick experiment (not kept): SdFat's PIO SDIO for the card, with `ide_dma` left out so SDIO had a block.
- **The card has to be power-cycled to leave SPI mode.** After the SPI firmware it wouldn't start over SDIO until it was taken out and put back. Going the other way, back to SPI, needs no power cycle.
- **25 MHz:** the card mounted and `HDDSYNTH.INI` was read, but 17 of 20 directory lookups failed to find `HDDSYNTH.IMG` (they always found `HDDBENCH.IMG`), so the drive fell back to the built-in disk.
- **12.5 MHz:** 20 of 20 lookups worked and the image was served (reads through the adapter in PIO stayed at 1.38 MB/s, the adapter's pace). But writing to `HDDBENCH.IMG` failed: a card error, `card=lost`, and Windows dropped the disk.
- At 6.25 MHz, four data lines would carry no more than SPI does at 25 MHz, so there's nothing to gain on these leads. DAT1 and DAT2 had never carried a signal before (SPI uses only DAT0); a bad joint on one of them would look like this, as would the leads' length (37.5 MHz SPI also failed on them).
- **So SDIO waits for the PCB** (short traces, and card detect on GPIO 40), or for a check of DAT1-DAT3 on the rig. The design below stands.

## The options considered

**A. One build, chosen at start-up:** serving an image means SDIO and no diagnostics; the built-in disk means diagnostics and SPI.

**B. Two builds, test and production:** the test build has the diagnostics and SPI, the production build has SDIO and none, chosen when flashing.

**C. One build, the blocks assigned at start-up by what's needed** (chosen):

| Block | Holds |
|---|---|
| 1 | `ide_bus`, always |
| 2 | SDIO while an image is served; otherwise the diagnostics |
| 3 | `ide_dma` while `HDDSYNTH.INI` offers DMA (`mode = mdma0-2`); otherwise the diagnostics |

The diagnostics go in whichever of blocks 2 and 3 is free. They're off only when an image is served **and** DMA is offered:

| Configuration | SDIO | DMA | Diagnostics |
|---|---|---|---|
| Built-in disk (soaks, timing tests) | no | if offered | yes |
| An image, PIO (the default; retro hosts without DMA) | yes | no | yes |
| An image with DMA (the fastest) | yes | yes | **no** |

### Why C

1. **One binary, so test results carry over.** Core 1's read timings move by tens of nanoseconds when its code moves in RAM (CLAUDE.md, and the 0.11.x A-B-A runs), and the margins have been that thin. With one build, core 1's code sits in the same place in every configuration, so a soak on the built-in disk speaks for the firmware serving an image. B gives that up: its two builds lay core 1 out differently, so a soak on one proves less about the other, and diagnosing a fault in the field means flashing the other build, which changes what's being diagnosed. B's `#if` paths also drift, and its releases grow to three UF2 files.
2. **The diagnostics stay on almost everywhere.** A turns them off whenever an image is served, including plain PIO on a retro machine, which is where slow reads and IDENTIFY checks matter most on an unfamiliar controller. C loses them only in the one configuration that needs all three blocks.
3. **The cost is small:** about half a day more than A, for the start-up allocator.

### What C gives up

With an image and DMA together:
- **The IORDY timer.** DMA moves only the data words; every command and status poll still goes through `ide_bus` with IORDY. A status read held past 1,250 ns would go unreported: no `SLOWREAD`, no `iordy_max_ns`. This is the real loss.
- **The IDENTIFY check at detection.** Minor: it has never caught a drive fault.
- Not data checking: the bus observer checks data sectors only on the built-in disk, whose contents it can predict, so that never ran on images.

Each part is still measured separately: DMA's timing on the built-in disk (block 2 is free there, so the diagnostics run alongside `ide_dma`), and SDIO's in PIO mode. What stays unmeasured is the combination: SDIO's card traffic and DMA bursts at the same moment, which could slow core 1's answers to status polls. (SdFat's SDIO uses no DMA channel: core 0 moves every word through the FIFOs, so the risk is less than first thought, but still unmeasured.) If that ever needs measuring, swap the card back to SPI for the run (block 2 then holds the diagnostics). If a fault appears in the fastest configuration, turning DMA off in `HDDSYNTH.INI` brings the diagnostics back, with SDIO still on.

## How to build it

### Rules

1. **Each PIO module says whether it started.** `iordy_timer`, `bus_observer`, `ide_dma` and the SD card get a `running()` (or `begin()` returns whether it got a block). Anything that reports or polls them checks it: the stats line shows `diag=off` (and no IORDY or observer fields) rather than misleading zeros, and `S` says why.
2. **One function decides all three blocks**, once at start-up, before core 1 starts the bus: from whether an image is served and whether DMA is offered. It logs its decision and the reason (`S`: `PIO bus=0 card=1(sdio) dma=2 diag=off: image and DMA`). Keep it a pure function of those two facts, so it can be unit-tested on the host.
3. **Core 1's code doesn't depend on the configuration.** The diagnostics run on core 0 and in PIO; core 1 only asks the IORDY timer to restart its timings when RESET# is released (`iordy_timer::requestReset()`), which becomes a no-op when the timer isn't running. Don't add `if (diagnostics)` branches to core 1's loop: they'd make its path through the code differ between configurations, which is what the single build is there to avoid.
4. **Claim blocks explicitly, not first-free.** Today each module calls `pio_claim_free_sm_and_add_program_for_gpio_range()`, which takes the first block with room, so the result depends on start-up order. SdFat's driver also tries `pio0`, then `pio1`, then `pio2`, and takes the first with 31 free instruction slots and all 4 state machines free; it can't be told which. The allocator should hand each module its block; with the library's driver, that means starting them in an order that leaves exactly the right block free when SdFat starts (or using our own copy of the driver; see [Before starting](#before-starting)).
5. **Each PIO module can stop.** `iordy_timer`, `bus_observer` and `ide_dma` have `begin()` but no `end()`. The console's block swap needs the diagnostics to free their state machines, their programs and `bus_observer`'s DMA channel, and to set the block's GPIO base back to 0 (SdFat's `end()` leaves it on 16).

### The card: SPI or SDIO, decided before the first command

**An SD card that has been put into SPI mode stays in it until its power is cycled.** It enters SPI mode on CMD0 with chip select (DAT3) low, which is how SdFat's SPI driver starts it. The rig has no way to cut the card's power, and the PCB has none either (rev 0.4 could add a switch; see the hardware notes of 28 Oct). So:

- **Start the card in SDIO from power-up whenever it will be served over SDIO**, and read `HDDSYNTH.INI` through SDIO too. Never touch it over SPI first.
- The allocator needs `HDDSYNTH.INI` to know whether an image is served, and needs the card to read it. Resolve it this way: at start-up, with a card present, take block 2 for SDIO and read the settings. If they say `image = none`, end SDIO (the card stays in SD mode, idle) and give block 2 to the diagnostics.
- **Console `C`, `E`, `I`, `K` while the diagnostics hold block 2** (built-in disk, card in): stop the diagnostics, start SDIO, do the file work, stop SDIO, start the diagnostics again. It's core 0 work, rare, and the diagnostics only miss that moment. Report it if it fails.
- **Re-mount detection** (`media_watch`, 0.13.0) probes a lost card with an SPI CMD0. Under SDIO it needs SDIO's own start-up instead: a card put back has been power-cycled, so it starts in SD mode. This must change in the same release as SDIO: the SPI probe would put a returning card into SPI mode. On the PCB, card detect (GPIO 40) can replace polling.
- **Flashing and rebooting don't power-cycle the card.** After any run that used SPI (an older firmware, or the SPI fallback), SDIO won't start until the card is replugged. When SDIO's start-up fails on a card that is present, say so loudly (`SD error: card may be in SPI mode; replug it`) rather than falling back quietly to the generated disk, where writes would go to RAM while the user thinks they're on the image. Switching the SPI fallback on and off costs a replug each time.

### Clock

SdFat's PIO SDIO runs four PIO cycles per SD clock. At 200 MHz that's a divider of 2 for 25 MHz (default speed: start here, on the rig too) and 1 for 50 MHz (high speed: needs CMD6 and the PCB's short traces).

### Steps

1. Add `running()` and `end()` to each PIO module and the stats/`S` reporting; make `iordy_timer::requestReset()` safe when not running, without a branch in core 1's loop. No behaviour change: everything still runs, since SDIO isn't in yet.
2. The allocator (a pure function plus its host unit tests), and explicit block claiming. Still no SDIO: check every configuration starts as before.
3. SDIO for the card at 25 MHz, from power-up, with `HDDSYNTH.INI` read through it; the console file commands' block swap; SDIO re-mount. Keep SPI selectable (a build flag or a hidden setting) for measuring and fallback.
4. On the rig: the write-and-verify test on `HDDBENCH.IMG` (`host/runs/tools/writeverify.py`), `-m card`, `-m speed` with write-behind off and on, and slices 3-5. Expect card writes to rise from 0.94 MB/s to about 1.2 (see above; more only if the store's timing breakdown says so). Reads won't change through this adapter until DMA lifts its PIO pace.
5. A soak on the built-in disk (diagnostics on) and a long run on an image (none), compared with the 0.16.0 soak.

## Before starting

In order:

1. **DMA on hardware** ([dma.md](dma.md), the two wires): it's what makes SDIO's read gain real, and its result could change this plan (see the last question below).
2. **Time the store path** on the card (command, transfer, busy wait) to see what SDIO can save on writes.
3. **Check DAT1 and DAT2 on the rig** (GPIO 37-38, and their 10 kΩ pull-ups): SPI never uses them, so a bad joint wouldn't show until SDIO.
4. **On the PCB (rev 0.4):** a switch for the card's power, which removes the SPI-mode trap, and card detect on GPIO 40.
5. **Our own copy of SdFat's SDIO driver** in `src/` (MIT licence, about 1,000 lines) rather than the library's, to fix what the review found:
   - it reserves all 4 state machines but uses 2, and picks its own block;
   - `USE_DEBUG_MODE 1` is hard-coded in the .cpp, so it prints to `Serial` on CRC and write errors, ignoring core 0's rule of printing only when USB serial has room, and into the trace;
   - it sets `input_sync_bypass` with `1 << m_clkPin`, undefined with the clock on GPIO 34, so most likely nothing is bypassed. Probably harmless at 25 MHz; it matters for 50 MHz. (`WAIT GPIO` is fine: the SDK corrects it for base 16 when the program is added.)

## Questions

- **Answered (28 Oct):** SdFat's PIO SDIO uses no RP2350 DMA channels. Core 0 moves every word through the FIFOs and computes the CRC itself, so core 0 is busy for each transfer, as with SPI today.
- **Answered (28 Oct):** it can be ended and restarted at run time. `end()` frees its state machines and removes its programs, and `begin()` does the same first. It leaves the block's GPIO base on 16 (rule 5).
- If a future change shrinks `ide_dma.pio` to 8 instructions or fewer (both programs together), it and the diagnostics could share a block and the trade-off goes away. Today they need 32.
