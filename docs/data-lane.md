# The data lane

**Status: on branch `data-lane` (0.29.0-2d.4), tested on the USB adapter.** The bus program used when `early_answers` and `isa` are both off. Data register reads are answered by PIO and DMA, without core 1.

## Why

Every register read is answered by core 1: the bus program pulls IORDY low, pushes the access to core 1, waits for the value, drives DD and lets IORDY go. ATA allows a drive to hold a read that way for 1,250 ns at most. Core 1 manages about 1,100 ns at worst, so there's little to spare, and with **early answers** (0.27.0's default) it didn't manage: each access cost core 1 two exchanges, a request at the chip select and a report after the strobe, including every data word. The FIFO was never empty while data flowed, and status reads queued behind the stream. A `-m speed` run held about 50,000 reads past 1,250 ns, up to 2,140 ns, and slice 1's detections held some up to 3,670 ns (3 Nov 2026).

PicoIDE and ZuluIDE, which emulate IDE drives on the RP2350 too, keep core 1 out of the data words altogether: DMA feeds the sector buffer to the bus, and core 1 handles only the registers. The data lane does the same.

## How

Two state machines in `ide_bus`'s PIO block, the only block whose outputs reach DD (`src/ide_bus.pio`):

- **`ide_bus_data`** (27 instructions) is `ide_bus` with one change. At the strobe it reads the address, and a data register read doesn't go to core 1: it raises PIO IRQ 4, waits 40 ns while `ide_data_in` drives the word, lets IORDY go, and releases DD when DIOR# rises. Register reads and every write, the data register's included, are served as `ide_bus` serves them.
- **`ide_data_in`** (5 instructions) takes the next word from its TX FIFO, waits for IRQ 4, drives DD and waits for DIOR# to rise. Its FIFO is joined, 8 deep, and a DMA channel keeps it filled from the device's block (16-bit transfers; the bus replicates them into both halves of the FIFO word, and `out pins, 16` takes the low half).

The two fill the block's 32 instructions, so there's no room for `iordy_watchdog`. ISA cards need the watchdog, so `isa = on` keeps `ide_bus`. `early_answers = on` keeps `ide_bus_early`.

A data read is answered about 50-90 ns after DIOR# falls, whatever core 1 is doing. That's in time even for a host that ignores IORDY, in PIO modes 0-2 (ATA makes IORDY compulsory from mode 3 up).

### Core 1's part, once per block (`ide_bus.cpp`)

1. **Start.** When the device has a data-in block (IDENTIFY, a sector), core 1 resets `ide_data_in` (empty FIFO, IRQ 4 clear) and starts the channel on the block **plus one spare word**. It does this before the host can see DRQ: the lane is synced before every register read is answered, after a command runs, and after a fetch completes, each time before INTRQ is updated.
2. **Move on early.** Once the channel has moved the spare word, every word the host has still to read is in `ide_data_in`'s FIFO or OSR (8 words), so the buffer is free. Core 1 catches the device up then (`consumeDataWords`: the next sector's DRQ, BSY while it's fetched, or the end), while the host reads the last words: about 9 µs on the USB adapter. Until the host has read them, STATUS and Alternate Status still read as the block's DRQ, reading them acknowledges nothing, and INTRQ waits. A register write meanwhile ends the block, since a host isn't allowed one.
3. **Finish.** `ide_data_in` pulls the spare word only once the last word's strobe has ended, so an empty FIFO with the channel idle means the host has read the block. Core 1 then starts the device's next block, if it has one: a few register writes, at most 230 ns (`lane_switch_max_ns`).

While a block is on the lane, core 1's loop waits in `serveLane()` for these steps, posting the read-ahead and taking its result meanwhile. It goes back to the bus at once when an access is waiting or the block is done, and carries on with its pass when a command is queued or RESET# changes.

## Results (USB adapter, built-in disk, PIO mode 3, 3 Nov 2026)

| | Early answers (0.28.0's default) | `ide_bus` (`isa = on`) | Data lane |
|---|---|---|---|
| Sequential read | 1.18 MB/s | 1.38-1.41 MB/s | **1.54 MB/s** |
| Random 4 KB read | 74.9 IOPS | 78-79 IOPS | 80 IOPS |
| Sequential write | 1.06 MB/s | 1.63-1.76 MB/s | 1.65 MB/s |
| Reads over 1,250 ns, one `-m speed` run | about 50,000 | 0 | about 10 |
| Worst read | 2,140 ns | 1,160-1,240 ns | 1,310 ns |

The lane reads faster than `ide_bus` because the adapter's PIO pace (about one word per 1.1 µs) no longer waits on core 1 for each word. The few reads over 1,250 ns are Alternate Status polls at the end of a WRITE command, while core 1 completes the last store. `ide_bus` has the same race, with 10-60 ns less on it. Taking core 1 out of register reads too (the register table in [no-iordy.md](no-iordy.md)) would end it.

## On a retro PC (the Evesham, 3 Nov 2026)

The Evesham (Athlon XP 2000+, PIO mode 3, `DOSBENCH.IMG` and HDDBENCH; [retro-test.md](retro-test.md#results-3-nov-2026-evening)):

| | Early answers | Data lane |
|---|---|---|
| Sequential read | 1.78 MB/s | **2.18 MB/s** |
| Random read | 1.09 ms | 0.98 ms |
| Sequential write | 1.24 MB/s | **1.98 MB/s** |
| Reads over 1,250 ns | 0 | about 1,400, all Status (mean 1,283 ns, worst 1,540) |
| Reads the host didn't wait for | 0 | 0 |

It detected and booted as on early answers, and ran two drives (device 1 an image) cleanly. Before it becomes the default:
- **Status reads over 1,250 ns:** core 1 answers register reads at the strobe, and on this PC more of them miss than through the USB adapter. Harmless here (the host waits for IORDY), but over ATA's limit.
- **No bus observer checks on the lane** (`bus_sectors=0`): data reads aren't sampled.
- **ISA machines** still need `isa = on` (no watchdog), as above.

## What it changes

- **Data reads don't reach core 1**, so the raw trace (console `t`) doesn't show them, and console `D` (delay data reads) and `F` (the fast lane) do nothing. `-m timing` skips on the lane. `s` shows `bus=lane`, `lane_blocks` and `lane_switch_max_ns`.
- **Not ISA-safe:** with no watchdog, an ISA card that selects the drive with no strobe (memory and refresh cycles aliasing its address) would stall. ISA machines need `isa = on`.
- **Hosts that ignore IORDY for register reads** (a PIIX in compatible timing while the BIOS detects the drive) still need `early_answers = on`. The lane covers their data reads only.

## Tests

- **Simulation** (`host/tests/test_data_lane_sim.py`, 92 tests): `pio_sim` runs several state machines in one block (`PioBlock`: shared pin outputs, directions and IRQ flags; WAIT on an IRQ flag) with a model of the DMA channel (`DmaToTx`). Covered:
  - register reads and writes in modes 0-3, at six phases of the PIO clock;
  - IDENTIFY and back-to-back sectors in modes 0-3 and on an ISA bus, every word from the lane and none reaching core 1;
  - the block seen as done only after the last strobe, with 8 words left when the channel finishes;
  - a status read mid-block, and a status read then data with CS0# held;
  - a host that ignores IORDY (modes 0-2);
  - data writes reaching core 1 with flow control;
  - a data read with no block (no hang, DD not left driven, the next block aligned);
  - a chip select with no strobe.
- **On the rig:** `-m speed` and slices 1-4 on the built-in disk, with the bus observer checking sectors on the cable.

## Found on the way

- **A command could be starved.** Core 1's loop reached its command check only when the bus program's FIFO was empty, and the adapter's back-to-back STATUS polls after a command came just faster than core 1 answered them. STATUS then read BSY forever. Core 1 now runs a queued command as soon as it has served the access, whatever is waiting. `ide_bus` had the same weakness, and was just quick enough to escape it.
- **The same, with a block on the lane:** a command written mid-block (the adapter does this when it abandons a half-read block) was left queued by `serveLane()`. It now returns to the pass for a command.
