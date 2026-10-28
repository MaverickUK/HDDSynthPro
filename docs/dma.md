# Multiword DMA

Why the drive needs DMA modes, how the data phase works, and the steps to get there.

## Why

The USB-IDE adapter paces PIO reads itself, at about one data word per 1.1 µs, whatever the mode: PIO mode 3 gained reads 2% over mode 0 (0.15.0, `speed-log.md`). In DMA the drive raises DMARQ when it has data and the host strobes it out, rather than polling, so the adapter's PIO pace and its 10 ms poll after each READ may not apply. DMA is also how faster hosts expect a drive to move data.

## How multiword DMA works on the bus

1. The host writes READ DMA (C8h) or WRITE DMA (CAh). The drive sets BSY while it gets ready.
2. The drive raises **DMARQ** (IDE pin 21). The host answers with **DMACK#** (pin 29), and strobes DIOR# (read) or DIOW# (write) once per word, **with the chip selects off**.
3. **There's no IORDY.** Each read word must be on DD within tE of DIOR# falling (50 ns in mode 2), and each write word is valid only tG (20 ns) before DIOW# rises.
4. To pause, the drive lowers DMARQ **as a strobe begins** (within tL, 35 ns in mode 2): the host finishes that word and stops until DMARQ rises again.
5. After the last word the drive clears DRQ, sets the status and raises INTRQ.

| Mode | Cycle t0 | Strobe tD | Read data tE (max) | Write setup tG | DMARQ off tL (max) |
|---|---|---|---|---|---|
| 0 | 480 ns | 215 ns | 150 ns | 100 ns | 120 / 40 ns |
| 1 | 150 ns | 80 ns | 60 ns | 30 ns | 40 ns |
| 2 | 120 ns | 70 ns | 50 ns | 20 ns | 35 ns |

## The design

**A second PIO program for the data phase** ([src/ide_dma.pio](../src/ide_dma.pio)): `ide_dma_read` and `ide_dma_write`, which fill a PIO block's 32 instructions between them. `ide_bus` keeps every register access and ignores DMA strobes (no chip select). Each burst:
- core 1 puts the number of words, less one, in the TX FIFO;
- the program raises DMARQ (side-set on GPIO 31);
- reading, RP2350 DMA streams the sector buffer into the TX FIFO, one word per entry (16-bit DMA writes); the program presents each word when DIOR# falls and releases DD when it rises, so DD is driven only during a strobe;
- writing, the program takes each word as DIOW# rises (DIOW# skips the input synchroniser and DD doesn't, so `in pins` sees DD from just before the edge), and DMA moves the RX FIFO into the sector buffer;
- DMARQ falls on the instruction after the last strobe is seen (side-set takes effect when an instruction issues, even one that then waits, so it can't go on the `wait` itself), and the program raises IRQ 0.

A burst is a sector: the device has the next one fetched (read-ahead) or a buffer free (write-behind), or DMARQ stays low until it has.

**DMACK# isn't watched.** A strobe with the chip selects off only comes after DMACK#, and DD is driven only while DIOR# is low, so a host that stops early never finds the bus driven. On the PCB DMACK# is on GPIO 3 for later use (Ultra DMA needs it).

**The data lines change hands each burst.** On the RP2350 a GPIO is driven by one PIO block at a time (its function select), and `ide_bus` fills one block, so the DMA programs go in another (the IORDY timer and bus observer share the second; the third is free). Core 1 hands DD0-15 to the DMA block before a burst and back after it, so a status read between bursts is still answered by `ide_bus`. Inputs aren't affected: every block reads every pin.

**DMARQ is driven only during a DMA command**: an input otherwise, which the host pulls down.

## Steps

1. **Simulator (done):** `ide_dma.pio` against a multiword DMA host (`host/hddproto/mdma_host_model.py`, `host/tests/test_dma_sim.py`) in modes 0-2: every word right over two-sector transfers, read data within tE and held tF, DMARQ falling 10 ns after each burst's last strobe begins, short and single-word bursts, DD released between strobes.
2. **Firmware (done, 0.16.0; not yet tried on hardware):** READ DMA and WRITE DMA in `ata_device` (unit-tested), IDENTIFY's multiword DMA words (49 bit 8, 63) and SET FEATURES 20h-22h; `ide_dma.cpp` (the DMA channel, handing DD between blocks, DMARQ only during a DMA command, tidying up when a command is cut short). Offered only when `HDDSYNTH.INI` says `mode = mdma0-2`: a host told the drive does DMA would wait for DMARQ forever where it isn't wired.
3. **Hardware:** the PCB, or the rig with the two wires below. Then: which mode the adapter picks, its DMA speed, data checks (slices 3-4 run in DMA when the drive offers it), and a soak. The bus observer can't check DMA data yet: it looks for data reads with a chip select.

## Adding the two wires to the BB48 rig

The PCB's pins, so the same firmware runs on both:

| IDE pin | Signal | GPIO | BB48 pin | Direction |
|---|---|---|---|---|
| 21 | DMARQ | 31 | EXT2-11 | out, only during a DMA command |
| 29 | DMACK# | 3 | EXT1-7 | in (not read yet) |

GPIO 3 carries the BB48's 2.2 kΩ pull-up to 3.3 V (R17, for I2C). The host drives DMACK#, so that's harmless. With the wires in, set `mode = mdma2` (console `E`, then `!`), and the adapter can choose DMA.
