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

MB/s here means 10^6 bytes per second. Worst read is the longest IORDY held a read during the test. Up to 0.8.3 the test didn't reset it first, so those figures also cover the adapter's detection and macOS's mount after the last replug, where a register read near 1.2 µs is usual; the traffic-only figure is in brackets where it was measured.

| Date | Firmware | Change | Transfer mode | Disk | Seq read | Random 4 KB read | Read command | Seq write | Worst read |
|---|---|---|---|---|---|---|---|---|---|
| 2026-10-25 | 0.7.3 | Baseline | PIO 0, 150 MHz | generated | 0.87 MB/s | 69 IOPS (14.4 ms) | 10.7 ms + 501 µs/sector | 1.56 MB/s | 1000 ns (953-1000) |
| 2026-10-25 | 0.8.1 | Core 1 fast lane for data reads | PIO 0, 150 MHz | generated | 1.05 MB/s | 73 IOPS (13.7 ms) | 10.8 ms + 398 µs/sector | 1.56 MB/s | 1146 ns (1120-1160) |
| 2026-10-25 | 0.8.2 | Access handling inlined again | PIO 0, 150 MHz | generated | 1.05 MB/s | 73 IOPS (13.7 ms) | 10.8 ms + 400 µs/sector | 1.56 MB/s | 1080 ns (1026-1053) |
| 2026-10-25 | 0.8.2 | (the same firmware) | PIO 0, 150 MHz, SD at 12 MHz | SD image | 0.59 MB/s | 61 IOPS (16.3 ms) | 10.4 ms + 780 µs/sector | (not run) | 1173 ns |
| 2026-10-25 | 0.8.3 | SD card's SPI clock 12 → 25 MHz | PIO 0, 150 MHz, SD at 25 MHz | SD image | 0.84 MB/s | 69 IOPS (14.5 ms) | 10.7 ms + 519 µs/sector | (not run) | 1186 ns (1026-1053) |
| 2026-10-25 | 0.8.6 | Read-ahead into a second sector buffer | PIO 0, 150 MHz, SD at 25 MHz | SD image | 1.26 MB/s | 76 IOPS (13.1 ms) | 10.9 ms + 318 µs/sector | (not run) | 1173 ns (1160-1173) |
| 2026-10-25 | 0.8.15 | Read-ahead's handover reworked for the worst read | PIO 0, 150 MHz | generated | 1.24 MB/s | 76 IOPS (13.1 ms) | 10.9 ms + 323 µs/sector | 1.56 MB/s | 1080 ns (1053-1080) |
| 2026-10-25 | 0.8.16 | Scratch image on the card (baseline for writes) | PIO 0, 150 MHz, SD at 25 MHz | SD bench image | 1.23 MB/s | 76 IOPS (13.2 ms) | 10.9 ms + 328 µs/sector | 0.92 MB/s | 1080 ns |
| 2026-10-25 | 0.8.20 | Write-behind on (not kept as the default) | PIO 0, 150 MHz, SD at 25 MHz | SD bench image | 1.26 MB/s | 76 IOPS (13.1 ms) | 10.9 ms + 318 µs/sector | 1.51 MB/s | 1546 ns |
| 2026-10-25 | 0.8.22 | Write-behind off by default | PIO 0, 150 MHz | generated | 1.27 MB/s | 76 IOPS (13.1 ms) | 11.0 ms + 314 µs/sector | 1.55 MB/s | 1106 ns |
| 2026-10-28 | 0.14.0 | 200 MHz (Windows rig, JMicron-ID adapter) | PIO 0, 200 MHz, SD at 25 MHz | SD image | 1.31 MB/s | 78 IOPS (12.9 ms) | 10.9 ms + 301 µs/sector | (bench image: 0.94 MB/s; 1.61 with write-behind) | 850 ns |
| 2026-10-28 | 0.15.0 | (mode 0 by HDDSYNTH.INI, for comparison) | PIO 0, 200 MHz | generated | 1.33 MB/s | 78 IOPS (12.9 ms) | 11.0 ms + 297 µs/sector | 1.58 MB/s | 910 ns |
| 2026-10-28 | 0.15.0 | PIO mode 3 (the adapter's choice) | PIO 3, 200 MHz | generated | 1.36 MB/s | 78 IOPS (12.8 ms) | 10.9 ms + 287 µs/sector | 1.79 MB/s | 920 ns |
| 2026-10-29 | 0.16.5 | Write-behind, re-measured (writeverify, 64 KB writes) | PIO 3, 200 MHz, SD at 25 MHz | SD bench image | — | — | — | 1.05 MB/s off, 1.65 on (192 MB verified) | 1040 ns |
| 2026-10-29 | 0.17.0 | DMA writes to the card, write-behind on (writeverify, 64 KB writes) | MDMA 2, 200 MHz, SD at 25 MHz | SD bench image | — | — | — | 2.23 MB/s (128 MB verified) | 1150 ns |
| 2026-10-30 | 0.18.1 | The bus observer on spot checks, so it no longer holds the drive back | MDMA 2, 200 MHz | generated | 7.70 MB/s | 1,100 IOPS (0.9 ms) | 0.22 ms + 64 µs/sector | 13.9 MB/s | 1110 ns |
| 2026-10-29 | 0.16.2 | Multiword DMA (`mode = mdma2`, DMARQ/DMACK# wired) | MDMA 2, 200 MHz | generated | 5.14 MB/s | 969 IOPS (1.0 ms) | 0.11 ms + 98 µs/sector | 9.76 MB/s | 1080 ns |

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

### 0.8.3: the SD card at 25 MHz

On the image disk each sector took 780 µs, 380 more than on the generated disk: moving 512 bytes over SPI at 12 MHz takes 341 µs, plus the card's command overhead, and core 0 only starts a fetch once the host has read the previous sector. At 25 MHz a sector takes 519 µs and sequential reads are 0.84 MB/s (from 0.59).
- Checked by reading the whole 64 MB image three times at 25 MHz with identical SHA-256s; at 12 MHz two reads also matched. (macOS writes to the volume when it mounts and unmounts it, so hashes are only compared with the volume unmounted, between remounts.)
- Not 37.5 MHz, the next step SPI0 can divide 150 MHz into: it would save only ~55 µs a sector on hand-wired leads, and reading ahead can hide the card's time completely.
- Tried on 27 Oct (0.8.24, `SD_SPI_MHZ = 50`): the card didn't start at all (SdFat error 30), so the drive fell back to the generated disk. The hand-wired leads to the breakout need shortening, or series resistors, before 37.5 MHz; it would mainly help writes (about 55 µs off a store of about 550).
- The worst read on the image looked like 1,173-1,186 ns, but those came from the detection after a replug, which the test then included. During traffic it's 1,026-1,053 ns, as on the generated disk: SD traffic doesn't slow core 1. The test now resets the read timings first.
- Between sectors the adapter waits for INTRQ rather than polling, so its pause is the fetch time plus its ~70 µs reaction: about 240 µs per sector here, against 73 on the generated disk. Reading ahead removes the fetch part.

### 0.8.6: read-ahead

The device has two sector buffers. While the host reads one, core 0 fills the other with the read's next sector, so when the host finishes, DRQ and INTRQ for the next sector come at once, with no BSY. A failed read-ahead is reported at the sector it belongs to.
- Image-disk reads: 1.26 MB/s, from 0.84; 318 µs a sector, from 519. Faster than the generated disk without read-ahead (400 µs): with no BSY between sectors, the adapter's pause shrinks too.
- It took three goes to keep the worst read inside 1,250 ns. 0.8.4 left the read-ahead's result in the mailbox until the block ended, so the device went BSY and an ALTSTATUS waited behind the bookkeeping (1,466 ns). 0.8.5 took results inside the fast lane (1,266 ns). 0.8.6 also posts the read-ahead from inside the lane, one small job per pass while the FIFO is empty, instead of between the adapter's ALTSTATUS and STATUS reads.
- The worst read on the image is now 1,160-1,173 ns, about 80 ns inside the limit; on the generated disk it was about 1,040 before read-ahead. To measure on the generated disk.
- Data: two full reads of the image identical; against the copy from before, only directory timestamps and macOS's own housekeeping files differ, which macOS rewrites at each mount.

### 0.8.15: the worst read back near 1 µs, read-ahead kept

With read-ahead the worst read had crept to 1,160-1,240 ns, too close to 1,250 for an overnight soak that fails at 1,250. Bisecting on the day: 0.8.2 still measured 960-1,053 ns, 0.8.6 1,160-1,173, so the cost came with read-ahead, yet switching read-ahead off (console `A`) didn't remove it. What did:
- posting the read-ahead after the block's first data word: at a sector boundary the adapter reads ALTSTATUS then STATUS straight after, and a post begun between them held the STATUS read;
- answering a status poll in the fast lane before catching the device up (which can't change the answer mid-block);
- keeping the lane's loop small: its rare case (an access that isn't a data read) out of line, so GCC keeps the loop's values in registers.

Two things that looked right made it worse, and were reverted: forcing the sector handover inline (it made `fetchDone` big, and the BSY path slower: 1,306-1,480 ns), and copying the PIO handles into locals (they spilled to the stack). Small changes to core 1's code move its timing by 100 ns or more through register allocation alone, so each one is measured on the rig.

Worst reads now, generated disk, three trials each:

| Read-ahead | Fast lane | Worst read |
|---|---|---|
| on (default) | on (default) | 1,053-1,080 ns |
| off | on | 1,053-1,066 ns |
| off | off | 1,226-1,240 ns (debug only) |

### Write-behind (0.8.16-0.8.22)

Writes to the SD card ran at 0.92 MB/s, 553 µs a sector against 327 into RAM: each store held the host under BSY. With write-behind the host sends the next sector into the other buffer while the last is stored, and writes reach 1.51 MB/s, every byte intact (300 mixed writes read back). But during writes some status reads were held 1.5-2.0 µs, over ATA's 1.25. Two theories didn't hold: deferring store results until the post-block hold ended made it worse (1,960 ns), which suggests the NS1081 polls Status during the hold. So it ships off (console `W` turns it on), until instrumentation shows what core 1 is doing when the slow read lands.

## Ceilings on this adapter

A 64 KB sequential read, if our part took no longer than each mode's cycle time allows, with the adapter's 10.7 ms per command and ~70 µs per sector unchanged:

| Transfer mode | Cycle time | Per sector on the bus | 64 KB read | Ceiling |
|---|---|---|---|---|
| PIO 0 (today: 1.59 µs a word) | 600 ns | 154 µs | 39 ms | 1.7 MB/s |
| PIO 4 | 120 ns | 31 µs | 23 ms | 2.8 MB/s |

The adapter's own waits cap it well before the bus does. DMA modes may behave differently: the adapter is known to use DMA with drives that offer it.
