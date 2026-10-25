# Speed log

How fast the emulated drive is, measured the same way after each change, so each step's gain shows. Newest entries go at the bottom of each table.

## How it's measured

`PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m speed` ([test_speed.py](../host/tests/test_speed.py)), on the Mac through the NS1081 USB-IDE adapter, with the raw bus trace off. The test:
- reads 32 MB sequentially in 64 KB requests (the adapter's largest command, 128 sectors);
- makes 500 random 4 KB reads;
- writes 16 MB in 64 KB requests into the RAM overlay window (generated disk only);
- times read commands of 1 to 128 sectors from the drive's own CMD/DONE lines, and fits them to "fixed cost per command + cost per sector".

It saves its table as `speed.txt` in the run's folder under `host/runs/`.

## Results

MB/s here means 10^6 bytes per second.

| Date | Firmware | Change | Transfer mode | Disk | Seq read | Random 4 KB read | Read command | Seq write | Worst read |
|---|---|---|---|---|---|---|---|---|---|
| 2026-10-25 | 0.7.3 | Baseline | PIO 0, 150 MHz | generated | 0.87 MB/s | 69 IOPS (14.4 ms) | 10.7 ms + 501 µs/sector | 1.56 MB/s | 1000 ns |
| 2026-10-25 | 0.8.1 | Core 1 fast lane for data reads | PIO 0, 150 MHz | generated | 1.05 MB/s | 73 IOPS (13.7 ms) | 10.8 ms + 398 µs/sector | 1.56 MB/s | 1146 ns |
| 2026-10-25 | 0.8.2 | Access handling inlined again | PIO 0, 150 MHz | generated | 1.05 MB/s | 73 IOPS (13.7 ms) | 10.8 ms + 400 µs/sector | 1.56 MB/s | 1040 ns |

## Where the time goes (0.7.3 baseline)

A 64 KB read takes 74.9 ms in the drive:

| Part | Time | Whose |
|---|---|---|
| The adapter's wait after each READ command | 10.7 ms | The adapter |
| 128 sectors × 256 data words × 1.59 µs | ~52 ms | Ours: mostly core 1's answer to each word |
| 128 gaps between sectors, ~70 µs each | ~9 ms | The adapter |

- **The adapter's 10 ms.** After a READ command the bus is silent for 10.3 ms, then the adapter reads status and the data. INTRQ is already asserted after a few tens of µs, and the adapter ignores it. Writes don't have this wait, which is why they're faster. A host that waits on INTRQ or polls status, as a retro PC does, won't have it either. It might change if IDENTIFY offers READ MULTIPLE or faster PIO modes.
- **The data words.** PIO mode 0 allows a word every 600 ns; ours take 1.59 µs, because IORDY holds the host while core 1 answers. 68% of data words were held longer than 400 ns, the longest 973 ns.
- **Between sectors.** The adapter reads ALTSTATUS once and finds the next sector ready, so fetching sectors further ahead wouldn't help on this rig.

## Changes

### 0.8.1: core 1's fast lane for data reads

While the host reads a DRQ block, core 1 spins on the bus program's FIFO and answers each data register read straight from the sector buffer, checking it with one mask-and-compare instead of the general register handling. Anything else (a status poll mid-block, a write, RESET#) leaves the lane and is served as before.
- Data words held longer than 400 ns: from 68% to about 3%. Each sector takes 398 µs instead of 501, and sequential reads are 21% faster.
- Writes are unchanged: the lane is for reads only.
- The worst read rose from 1,000 to 1,146 ns, still inside ATA's 1,250. It's the same with the lane switched off (console `F`), so it isn't the lane: it's an ALTSTATUS poll between commands meeting one of core 1's bookkeeping steps.
- On the way, 0.8.0 lost about 1 µs a read to an out-of-line `std::atomic<bool>::load` that GCC placed in flash. `scripts/check_core1_ram.py` now fails the build if core 1 can reach any function in flash.

### 0.8.2: the worst read back near 1 µs

Splitting the general access handling out of `serviceOnce` for the fast lane had cost every access a function call: about 100 ns of register saves and restores. Forcing it inline again brought the worst read from 1,146 to a steady 1,026-1,053 ns over six trials, lane on or off, about 200 ns inside ATA's 1,250. Throughput is unchanged. Inlining it made GCC call the small decode helpers out of line, from flash, which `check_core1_ram.py` caught; those are always inlined now too.

### Tried: READ/WRITE MULTIPLE (not kept)

A probe build offered up to 16 sectors per DRQ block in IDENTIFY (word 47) and accepted SET MULTIPLE MODE. The NS1081 ignored it: at detection it sent IDENTIFY and two SET FEATURES, and every read after that was a plain READ SECTORS. So multiple mode can't remove its pause between sectors on this rig. Retro PCs often use it, so it stays on the list for them.

### Where the time goes now (0.8.2)

A sector takes about 400 µs: about 330 µs for the 256 words (1.29 µs each) and about 70 µs of the adapter's pause before the next one. Only about 3% of words are now held past 400 ns, so most of each word's 1.29 µs is the adapter's own PIO mode 0 cycle, not us. On this rig the remaining levers are:
- faster PIO modes, which the adapter would use if IDENTIFY offered them (it sets the mode with SET FEATURES at detection). That changes the bus timing, so it waits for the bus observer's answer on the rare mismatch;
- the SD card path, for the image disk: core 0's fetches from the card are slower than from the generated disk.

## Ceilings on this adapter

A 64 KB sequential read, if our part took no longer than each mode's cycle time allows, with the adapter's 10.7 ms per command and ~70 µs per sector unchanged:

| Transfer mode | Cycle time | Per sector on the bus | 64 KB read | Ceiling |
|---|---|---|---|---|
| PIO 0 (today: 1.59 µs a word) | 600 ns | 154 µs | 39 ms | 1.7 MB/s |
| PIO 4 | 120 ns | 31 µs | 23 ms | 2.8 MB/s |

The adapter's own waits cap it well before the bus does. DMA modes may behave differently: the adapter is known to use DMA with drives that offer it.
