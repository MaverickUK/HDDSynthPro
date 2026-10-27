# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

## [0.10.1] - 2026-10-27

The activity link's loopback test.

### Added
- **Console `U`**: with a jumper from GPIO 0 (TX) to GPIO 1 (RX), reads back what the link sends for 1 s and prints `LOOPBACK pass|fail|no-data sent= valid= rejected= errors= bytes= last=`. It passes when every sentence sent arrived valid, with no UART errors. GPIO 1 is the UART's input, with a pull-up, only while the test runs, and goes back as it was afterwards. It runs on core 0 without waiting, and resets the UART between sentences as it starts, so what the UART read before (breaks from the unconnected input, and the overrun mark that follows) doesn't count.
- `activity_link::parseLine` and `Receiver`, a byte-at-a-time receiver with the reference parser's rules (`host/hddproto/activity_link.py`), host-tested with the cases the synth's brief lists. The HDD Synth's receiver can be a copy.
- `host/tests/test_link_loopback.py` (`-m link`): `U` idle and during reads.

### Changed
- The synth's brief no longer says to reject versions other than 1: a later version only adds fields at the end, so a receiver accepts it and ignores those fields, as both reference parsers do.

### On the rig (Windows, no jumper)
- `U` reports `no-data` (5 sent, nothing read, no errors) and the link carries on. With the UART's internal loopback (a throwaway build that set its LBE bit), `U` passed three times in a row, idle and during mixed reads from the SD image (5-6 sentences, all valid). Before the reset at the start, the second run failed on one overrun error.

## [0.10.0] - 2026-10-27

Settings on the SD card.

### Added
- **`HDDSYNTH.INI`** (docs/settings.md, `src/drive_config.*`): `image = <file>` chooses the disk image, and `mode = auto` (or `pio0`-`pio4`, `mdma0`-`mdma2`, `udma0`-`udma6`) the transfer mode to offer. Plain `key = value` text with comments; Notepad's CRLF and byte-order mark are fine. Missing file or keys mean the defaults (`HDDSYNTH.IMG`, `auto`), so a card without it behaves as before. Mistakes are reported and skipped, never fatal.
- The disk, in order: `HDDBENCH.IMG` after console `J`; the configured image; `HDDSYNTH.IMG` if that can't be used (not found, or empty); the generated disk. With no card the images aren't tried, so start-up without one still takes one 2-second wait.
- Modes other than PIO 0 are accepted with a warning and mean `auto` until they exist, so settings written now keep working.
- Console: `S` adds `CONFIG` lines (the settings, the image served, why a configured image wasn't used, warnings); `C` creates the file with the defaults; `E` then `key = value` sets one setting, keeping the file's comments and line endings; `!` restarts. `s` shows `image_name=` and `config=`.
- The firmware may now write `HDDSYNTH.INI` (only with `C` or `E`), as well as the image it serves and the images `I`/`K` create.

### On the rig (Windows)
- Without the file: as before. `C`, then `E image = HDDBENCH.IMG` and a restart: served the scratch image. `image = MISSING.IMG`: fell back to `HDDSYNTH.IMG`, reported as `MISSING.IMG: not found`. `mode = udma5`: warned, used auto. An unknown key was refused. `J` still wins over the file. Left the card with the default file.
- Not yet tried on the rig: starting with no card (needs the card pulled).

## [0.9.0] - 2026-10-27

The activity link to an HDD Synth, this side.

### Added
- **The activity link** (`src/activity_link.*`, docs/activity-link.md): the drive sends `$HDA,1,<busy>,<class>,<op>*<checksum>` sentences on UART0 TX (GPIO 0, 115200 8N1), on every change and every 200 ms, so an HDD Synth can play a different sound for sequential transfers than for random access. Busy is the LED's own flag; the class comes from the activity classifier; op is whether the last media command read or wrote.
- Driven through the Pico SDK, transmit only: a sentence is written only when the 32-byte TX FIFO is empty, so core 0 never waits on it. GPIO 1 (UART0 RX) is reserved and untouched.
- Console `L` turns it off and on; `s` shows `link=`, `link_sent=` and `link_last=` (the last sentence).
- `host/hddproto/activity_link.py`, the receiver's reference parser (it ignores bad checksums, other sentences and lines over 80 bytes, and accepts extra fields), with `sim` tests; C++ tests for the format, the checksum (the spec's known answers, shared with the Python tests) and the send rules.
- `test_ns1081_detection`: the adapter's detection sequence, from the drive's traces, as a host test.

### On the rig (Windows, SD scratch image)
- Sends: an idle heartbeat every 200 ms (`$HDA,1,0,I,-*28`), and 541 sentences over the speed and activity tests.
- No cost to the bus: reads 1.26 MB/s, 76 IOPS, writes 0.92 MB/s, worst read 1,120 ns, no slow reads, as 0.8.24. Core 1's code is unchanged and sits where it did.
- The activity test passes (each phase 95.5-99% in the right state).
- Not yet received by anything: the loopback test needs a jumper from GPIO 0 to GPIO 1.

## [0.8.24] - 2026-10-26

Write-behind's slow status reads traced to core 0, and fixed there.

### Fixed
- **Core 0 could spend hundreds of microseconds draining the IORDY timer's reports** (one for every read held over 400 ns). `iordy_timer::poll()` drained until the FIFO was empty, and since 0.8.23 converted every report to nanoseconds (a 64-bit division) to find the ones past 1,250 ns. When reports came every few microseconds it fell behind and stayed there. Read-aheads were picked up late, so the host found BSY at block ends and backed off (reads 0.87 instead of 1.27 MB/s), and the polling of the PIO's FIFO held up core 1's own PIO accesses. Each report is now compared with a limit worked out once at start-up, only reads past it are converted, and one call takes at most 8 reports. If reports come faster than that, the PIO drops the extra ones (it never waits), so `iordy_slow` can undercount. Core 1's code is unchanged.

### On the rig (Windows, generated disk)
- Write-behind on, 3-minute soaks: 0 slow reads (0.8.23: 32 and 39, worst 1,280 ns).
- Write-behind on, 30-minute soak: 1,609 MB, 55,660 commands, no mismatches, no resets, 0 slow reads; worst read 1,160 ns (STATUS); 2.76 million sectors checked on the cable, none bad.
- Write-behind on, 8-hour soak (27 Oct): 22.1 GB, 760,100 commands, clean; worst read 1,173 ns.
- **On the SD card (`HDDBENCH.IMG`, 16 MB of 64 KB writes) write-behind still holds reads**: writes 1.47 MB/s instead of 0.90, but about 1,600-2,800 ALTSTATUS reads per run past 1,250 ns, worst 1.5-2.0 us; off, none. There a store (about 550 us) outlasts the next block, so the drive waits in BSY for it at every block end, and core 0 is busy with SPI while the host reads status. Ruled out on the rig: contention on the fast peripheral bus (the bus counters show less with write-behind on), core 1's `time_us_32()` stalling behind core 0's SPI traffic on the APB (never over 12 cycles), and DMA (SdFat polls the SPI). Posting the next store before raising INTRQ, instead of in the following pass, didn't help and made the adapter hang (a write never completed; reverted). Next: watch it on a logic analyser, with GPIO markers from core 1.
- Reads 1.27 MB/s, as 0.8.23 (4 MB sequential, 64 KB requests).
- Found on the way, and not kept: inlining `Device::intrq()` or adding an inline data-word write changed only where core 1's code sat in RAM, yet held about 40% of data reads past 400 ns (3% before). That flood of reports is what exposed the core 0 problem. Caching the PIO FIFO pointers made the fast lane faster than ever, and with it the host reset the drive twice in 6 minutes (a command never completed); not understood yet.

## [0.8.23] - 2026-10-26

What holds status reads past 1.25 us with write-behind on: measured, not yet fixed.

### Added
- **`SLOWREAD t register ns`** for every read IORDY held past ATA's 1,250 ns (`cfg::IORDY_LIMIT_NS`), from the IORDY timer. The time is when core 0 collected the report, up to a sector's store after the read. The Python parser keeps them (`Trace.slow_reads`).
- `s` shows `write_behind=`.

### On the rig (Windows, generated disk, 3-minute soaks)
- Write-behind on: 32 and 39 slow reads in two runs, all ALTSTATUS, all during writes, median 1,253 ns, worst 1,280. Write-behind off: none in 13.5 hours.
- A pass probe (core 1 timing each pass of its loop by what it did, with its SysTick) showed bookkeeping passes of 400-730 ns even idle, and passes over 1 us for finishing a written sector, posting its store and taking the result. But its own bookkeeping pushed slow reads to 2,000-16,000 per run, so it was removed rather than kept as a tool that misleads.
- Finishing the written sector later instead of at its last word made it worse: as core 1's next job, 931 slow reads (ALTSTATUS); after the host's ALTSTATUS read, 18,820, and after its STATUS read, 18,431 (now STATUS). The work after a block (finish the sector, post the store, take the result) only fits before both status reads if it starts at once. Not kept.
- Next: make core 1's loop passes cheaper.

## [0.8.22] - 2026-10-25

A scratch image for benchmarks, and write-behind (built, measured, off by default). 0.8.16 to 0.8.21 were rig builds, not released.

### Added
- **`HDDBENCH.IMG`, a scratch image for tests that write freely.** Console `K` creates it (64 MB, zeros; refuses if it exists or there's no room). Console `J` restarts the drive serving it, or back: the choice is kept in a watchdog scratch register, which a power cycle clears, so after power-off the drive always serves `HDDSYNTH.IMG`. `s` shows `disk=bench`; `S` shows `image=` and `bench=` (open, present or none). The speed test measures writes on it; the soak and slices 3-4 still need the generated disk, and `require_generated_disk` now insists on it.
- **Write-behind** (`Device::setWriteBehind`, console `W`): the host sends the next sector into the other buffer while core 0 stores the last, and the command completes only when every sector is stored. A failed store is reported, at its sector, once the block in progress ends. Unit-tested, including under ThreadSanitizer. **Off by default**: on the rig it lifts writes to the SD card from 0.92 to 1.51 MB/s with every byte intact, but it holds some status reads during writes 1.5-2.0 µs (ATA allows 1.25), not yet understood.
- After each data-out block but the last, the drive shows BSY for at least 10 µs (`cfg::DATA_OUT_BSY_US`) before DRQ and INTRQ ask for the next one. The NS1081 reads Status straight after a block, clearing any interrupt, then waits for INTRQ; without the hold, write-behind sometimes left it waiting forever.

### Fixed
- `SpscRing::push`/`pop` and `FetchMailbox::post`/`takeResult` are always inlined; a new call site made GCC call copies in flash (caught by `check_core1_ram.py`).

### On the rig
- SD card writes on the bench image: 0.92 MB/s (553 µs a sector), against 1.55 MB/s into the RAM overlay. Creating the 64 MB image took 30 s.
- Generated disk, write-behind off: reads 1.27 MB/s, writes 1.55 MB/s, worst read 1,106-1,120 ns; 5-minute soak clean, 434,799 sectors checked on the cable.
- Handling the mailbox in the middle of every block (0.8.17) made the adapter stop after SET FEATURES at detection, twice; posting stores only (0.8.19) didn't. Not understood.

## [0.8.15] - 2026-10-25

The SD path made safe for real use, and the worst read back near 1 µs with read-ahead kept. 0.8.7 to 0.8.14 were rig builds, not released.

### Added
- **FLUSH CACHE flushes the storage.** The device goes BSY and core 0 calls `BlockSource::flush()`; on the image that is SdFat's `sync()`, which also waits for the card. A failed flush aborts.
- **IDENTIFY declares a write cache and FLUSH CACHE** (words 82/85 bit 5, 83/86 bit 12), so hosts that flush know to. **SET FEATURES 02h/82h** turn the write cache on and off; off, each written sector is synced before the write completes. A hardware reset turns it back on.
- SET FEATURES trace lines show the subcommand and its count.
- `disk_ms` in `s`: how long choosing the disk took at boot.
- Console `A` switches read-ahead (debug, for comparisons, like `F`).

### Changed
- Core 1's fast lane posts the read-ahead only after the block's first data word, answers a status poll before catching the device up, and handles anything but a data read out of line, so its loop keeps its values in registers.

### Fixed
- The worst read with read-ahead was 1,160-1,240 ns; it's 1,053-1,080 again (0.8.2: 960-1,053), with read-ahead on or off. Found by bisecting: 0.8.2 still measured 960-1,053 on the day, 0.8.6 1,160-1,173.

### On the rig
- The NS1081 never sends FLUSH CACHE, not on unmount and not for `F_FULLFSYNC`: the flush is for other hosts.
- At detection it asks for PIO flow-control mode 4 (SET FEATURES 03h, 0Ch) before falling back to mode 0, which we refuse because IDENTIFY doesn't offer it.
- With no card the drive gives up after 2.0 s (SdFat's timeout) and serves the generated disk.
- Generated disk: 1.24 MB/s, worst read 1,080 ns; 5-minute soak clean, the bus observer checked 431,217 sectors.

## [0.8.6] - 2026-10-25

### Added
- **Read-ahead.** The device keeps two sector buffers: while the host reads one, core 0 fills the other with the read's next sector, and DRQ for it comes as soon as the host finishes. A failed read-ahead is reported at its own sector; a late one after a reset is ignored. Core 1's fast lane posts the read-ahead and takes its result while it waits between words, one small job per pass.
- Unit tests for read-ahead (hidden fetch, separate buffers, failure at the right sector, stale after reset); also clean under ThreadSanitizer.

### On the rig
- Image-disk reads 1.26 MB/s (from 0.84), 318 µs a sector (from 519). Worst read 1,160-1,173 ns. Two full reads of the image identical.

## [0.8.3] - 2026-10-25

### Changed
- **The SD card's SPI clock is 25 MHz** (`cfg::SD_SPI_MHZ`, from 12). Image-disk reads go from 0.59 to 0.84 MB/s; three full reads of the image gave identical hashes.

## [0.8.2] - 2026-10-25

### Fixed
- **The worst read is back near 1 µs** (1,026-1,053 ns, from 1,146 in 0.8.1). Core 1's general access handling is inlined into `serviceOnce` and the fast lane again, instead of costing every access a call. The bus decode helpers (`bus_sample.h`) and `Device::peek` are always inlined, since GCC otherwise calls copies in flash, as `check_core1_ram.py` showed.

### On the rig
- 5-minute soak: no mismatches; the bus observer checked 381,436 sectors.

## [0.8.1] - 2026-10-25

The start of the speed work, logged in `docs/speed-log.md`: sequential reads from 0.87 to 1.05 MB/s.

### Added
- **Core 1's fast lane for data reads** (`ide_bus::serveDataIn`, `Device::dataInRemaining`/`consumeDataWords`): while the host reads a DRQ block, core 1 answers each data register read straight from the buffer. Anything else leaves the lane and is served as before. Off while the raw trace or the debug delay is on; console `F` switches it for comparisons.
- **`scripts/check_core1_ram.py`**: fails if anything core 1 calls from `loop1` (other than start-up and reset one-offs) is in flash. `scripts/rig.sh` runs it before flashing the drive build, and so does CI.
- `test -m speed`: the benchmark behind the speed log.

### Fixed
- 0.8.0 (not released) held reads up to 2.6 µs: GCC called an out-of-line `std::atomic<bool>::load`, which lives in flash. Core 1's shared flags are now `SharedFlag`, whose load and store are always inlined.

### On the rig
- Sequential reads 1.05 MB/s (from 0.87), 398 µs per sector (from 501); writes unchanged at 1.56 MB/s. Worst read 1,146 ns (from 1,000), the same with the lane off.
- 10-minute soak: no mismatches; the bus observer checked 762,610 sectors.

## [0.7.3] - 2026-10-25

### Added
- **A bus observer** (`bus_observer.pio`, `bus_observer.*`, `sector_check.*`): a third PIO program samples DD0-15 and the register bits just before every DIOR# rise, DMA copies the samples to a ring in RAM, and core 0 checks each data sector against the generated disk. `s` shows `bus_sectors`, `bus_bad`, `bus_skipped` and `bus_gaps`; a wrong sector prints a `BUSERR` line (LBA, first wrong word, bits lost and gained). It tells whether a mismatch the host sees was already wrong on the cable.
- The soak labels each mismatch "wrong on the cable" (with the `BUSERR` line) or "right on the cable", and logs the observer's counts.

### On the rig
- 3 minutes of soak traffic: 207,564 sectors checked, none wrong, no samples missed. The worst read rose from about 955 ns to 1,000 ns, probably the observer's DMA on the bus.

## [0.7.2] - 2026-10-25

Diagnostics for the rare read mismatch the overnight soak found: one sector in 8 hours (17 GB), always the first sector of the second command when a long read is split at 128 sectors, with bits that should be 1 read as 0.

### Added
- **The IORDY timer counts reads that start while IORDY is still high** (`iordy_early`, `iordy_early_reg` in `s`), when the host might not wait for the data. On the rig: none in 15 minutes of soak traffic, so the adapter's strobe recovery isn't racing the bus program's re-arm.
- **Console `D` and a digit** (debug): core 1 answers every data read that many × 250 ns later. `-m timing` steps it from 0 to 2,250 ns and checks the reads. On the rig the adapter waited for reads held up to 3.2 µs with no errors, so it doesn't give up on IORDY near our worst read (about 1 µs).
- The host model's strobe recovery (`Mode0.t2i`) for back-to-back accesses with CS held.

### Changed
- The soak waits 11 minutes once an hour, longer than macOS's disk sleep, so the drive goes into standby and the next access wakes it; it logs standbys, wakes and early reads. Overnight: 6 standbys and wakes, all clean.

## [0.7.1] - 2026-10-24

### Added
- **The BB48's LED (GPIO 25) is the drive's activity light**, like a hard disk's: lit while a command runs, and for at least 25 ms after any command starts (`cfg::LED_MIN_MS`), so a command of a few microseconds still shows. Core 1 counts commands as they start; core 0 drives the LED, so the bus loop gains two relaxed atomic stores per command.

### Documentation
- Brought the docs up to date with slices 4–5 on hardware: the README status, `docs/bringup.md` (the slice 1 IORDY limit, slice 4, the soak, an SD card section, the adapter's quirks), `docs/plan.md` (SD pins, slice status), `docs/wiring.md` (GPIO 25) and CLAUDE.md.

## [0.7.0] - 2026-10-24

The drive's disk can now live on the SD card (slice 5): macOS formatted it, mounted it, and the files survived a power cycle.

### Added
- **A disk image on the SD card, `HDDSYNTH.IMG`.** At boot core 0 mounts the card and, if the image is there, serves it as the disk (`ImageDisk`, over the `ImageFile` interface; SdFat over SPI underneath). Without a card or an image the drive uses the generated disk with its RAM overlay, as before.
  - Core 1 waits at boot for core 0 to choose the disk, so IDENTIFY reports the image's real size. Until then the bus pins stay inputs.
  - Console `s` shows `disk=image|generated` and `sectors=`; `S` adds `image=open|present|none`.
- **Console `I`** creates `HDDSYNTH.IMG`: 64 MB, one contiguous run, zero-filled (about 50 s). It refuses if the file exists or the card lacks room, and never touches any other file. The card's other contents are never formatted, deleted or changed.
- Host tests for `ImageDisk`: byte order, the last sector, out-of-range access, SD errors as failed fetches, and a write-then-read through the drive.

### Changed
- Slice 3, slice 4 and the soak skip when the drive serves an image. Their reads compare against the generated pattern, and slice 4 and the soak write to sectors 100,000–100,255, which would corrupt a formatted image. The read-write raw-disk fixture refuses an image outright.

### On the rig
- macOS partitioned and formatted the image (MBR, FAT32), mounted `HDDSYNTH`, and two files written to it, one a byte-exact copy of the README, were intact after a full power cycle. Worst read throughout: 993 ns.

## [0.6.4] - 2026-10-24

The first step of slice 5: the microSD card answers.

### Added
- **The microSD breakout's pin map** (`cfg::PIN_SD_*`): CLK, CMD and DAT0–DAT3 on GPIO 34–39, so the same leads serve SPI now (SPI0's pins, DAT3 as chip select) and 4-bit SDIO later (DAT0–3 consecutive). The breakout has no pull-ups; 10 kΩ ones are fitted on CMD and DAT0–DAT3.
- **Console `S`** (drive build): starts the card over SPI at 12 MHz with SdFat and reports its type, size and filesystem. It only reads. It refuses to start if SPI0 rejects the SD pins, because SPI0's default pins on this board are IDE data lines (GPIO 4–7).
- `docs/wiring-sd.html` (interactive) and a section in `docs/wiring.md`.

### On the rig
- `SD ok type=SDHC/SDXC size_mb=30436 fs=FAT32` on a 32 GB card, first time.

## [0.6.3] - 2026-10-24

### Fixed
- **The status poll straight after a command could wait up to 1.6 µs** (ATA allows 1.25 µs). The IORDY timer's register field showed it was always a STATUS read just after IDENTIFY or SET FEATURES.
  - Core 1 now runs a queued command and goes straight back to the bus. The CMD trace line is recorded later, with the time taken when the command ran.
  - Core 1 records at most one trace line per bookkeeping step, so a fast command's CMD and DONE lines no longer come back to back.
  - On the rig the worst read around detection is now 993–1006 ns over the last replugs, down from 1.03–2.2 µs.

### Changed
- Core 1 has high priority on the bus fabric (`BUSCTRL_BUS_PRIORITY`), so core 0's USB and other traffic can't delay its PIO accesses.
- Console `s` adds `bus_contested` and `bus_stalls`: contested accesses and stall cycles on the fast peripheral bus (where the PIO is), from the bus fabric's performance counters. They reset with the read timings. During detection they total about 3,000 stall cycles (20 µs), too few to matter.

## [0.6.2] - 2026-10-24

### Changed
- **The IORDY timer names the slow read.** Each report carries the register the read was for (DA2–0, CS0#, CS1#, sampled as DIOR# falls), and console `s` adds `iordy_max_reg` and `iordy_max_at_us` (when core 0 collected the worst report). The PIO simulator models `in`.

### Found on the rig
- The worst read around detection is a DATA read (2.2 µs on one replug), not a status poll. An earlier marker also put a slow read at the start of IDENTIFY's data.
- The NS1081 can't be made to re-detect the drive without a physical replug. A `uhubctl` replug doesn't reset its IDE side. A drive that only answers BSY makes it soft-reset at once. If the drive also ignores the soft reset, the adapter gives up on it until replugged.

## [0.6.1] - 2026-10-24

### Changed
- **A Command write only sets BSY on the bus** (`Device::queueCommand`); core 1 runs the command as its next job (`runQueuedCommand`). A status poll straight after the write sees BSY at once, as with a real drive. `write(StatusCommand, ...)` still runs a command at once, for tests and other callers.
- **Mid-sector, core 1 only serves the bus and watches RESET#.** While the host streams a DRQ block's 256 words (`Device::transferring()`), the other bookkeeping waits.
- The trace's CMD line is recorded when the command starts, from the queue.

### Measured on the rig
- Slices 1–4 and the activity test pass. Slice 1's IORDY check passes on some replugs and not others: the worst read around detection varies from 1.03 to 1.35 µs between runs (ATA allows 1.25 µs). In heavy traffic it stays at about 1 µs. The cause of the detection outliers is not yet known; these changes didn't move them.

## [0.6.0] - 2026-10-24

The drive is now a read/write IDE disk in PIO mode 0 (slice 4).

### Added
- **WRITE SECTORS** (30h, and the obsolete 31h). The data-out handshake follows ATA:
  - DRQ without an interrupt for the first sector;
  - BSY while core 0 stores each sector;
  - then DRQ and an interrupt for the next sector, or, after the last, completion with an interrupt.
  - A store that fails ends the command with ABRT and the failing LBA in the task file.
  - The handshake runs through the same `Fetch` and mailbox as reads, with `Fetch::store` set.
- **A RAM overlay disk** (`RamOverlayDisk`, `src/block_source.h`): writes are kept in RAM on top of the generated disk, for up to 512 distinct sectors (256 KB, `cfg::RAM_DISK_SECTORS`). Everything else reads the generated pattern; a write to a new sector when the overlay is full fails. `BlockSource` gains `write()`.
- **Write flow control in the bus program.** Writes don't wait for core 1, so a 256-word sector could outrun it and lose words. If the PIO's RX FIFO is full, IORDY now stays low until it has room, so the host waits instead.
  - In simulation, a core 1 taking 1 µs per word lost 98 of 256 words before this change and none after; the longest IORDY hold is 906 ns.
  - `ide_bus.pio` is now 32 instructions, a PIO block's whole instruction memory.
- Slice 4 rig test (`-m slice4`): 1,000 write-then-read cycles of random data in a 256-sector window, a long write, and the sectors around the window unchanged. It writes the generated content back afterwards, so slice 3 still passes.
- The PIO simulator models `mov x, status` (the RX FIFO level), and core 1 taking time over each access (`Core1(service_ns=...)`).

### Measured on the rig
- **Slice 4 passes:** 16,000 sectors written and read back at random, no mismatches, 0.5 MB/s each way.
- **Slices 3 and 4 pass together.** Over about 245 million reads, the longest IORDY hold was 986 ns, inside ATA's 1250 ns. The 1.28 µs seen in 0.5.5 happens only around detection.

## [0.5.5] - 2026-10-24

### Changed
- **Core 1 puts the bus first.** It serves every access waiting in the FIFO back to back, and only when the FIFO is empty does it do one small bookkeeping step before looking again.
- **Reads are answered before their side effects.** `Device::read()` is split into `peek()` (inline, no side effects) and `consumeRead()`. The bus layer hands `peek()`'s value to the PIO, then calls `consumeRead()`. The register is looked up from a 32-entry table instead of being decoded on every access.
- Slice 1 checks the IORDY timer against ATA's 1250 ns (tB). Core 1's own `read_max_ns` estimate is gone, with its cycle-counter setup.

### Measured on the rig
- The longest read went from 3.7 µs to 1.79 µs (tight loop), then to 1.28 µs (lean read path). Typical reads are still over 400 ns: what is left is fixed overhead around core 1. The next step is DMA-fed data reads, which would bypass core 1.

## [0.5.4] - 2026-10-24

### Added
- **An IORDY timer** (`src/iordy_timer.pio`, drive build): a second PIO program that only reads pins and times each read from the host's side, from DIOR# falling until IORDY is high again. Reads held longer than `cfg::IORDY_REPORT_NS` (400 ns) are counted and timed; console `s` shows `iordy_slow` and `iordy_max_ns`.
  - It runs in simulation too (`tests/test_iordy_timer_sim.py`).

### Changed
- `read_max_ns` and the IORDY timer both restart when the host releases RESET#. While the host powers up its strobes float low, which looked like one long read (up to 11 µs).

### Measured on the rig
- Nearly every read is held over 400 ns, and about 45% over 1 µs; the worst is 3.7 µs, against ATA's 1250 ns (tB). The NS1081 waits for IORDY, so nothing fails, but core 1 is too slow to answer reads.
- `read_max_ns`, core 1's own estimate, reads about 7 µs: it overestimates and will be replaced by the timer.

## [0.5.3] - 2026-10-23

### Fixed
- **The NS1081 hung after waking the drive from standby**, failing every read until it was replugged. When IDLE IMMEDIATE (or IDLE) wakes the drive from standby, it now shows BSY for 50 µs (`cfg::WAKE_BUSY_US`) and then completes with an interrupt, as a CF card does.
  - The adapter reads Status straight after the command, so an interrupt raised at once was cleared before it waited for it.
  - Found by listening to a CF card on the adapter's 44-pin header with the listen build.
  - After the wake the adapter waits about 2.5 s, as it does with the CF card, then reads normally.

### Verified on the rig
- **Slice 3 passes:** a full 64 MB read (0.65 MB/s) and random reads, with no mismatches, resets or errors.
- With the raw bus trace off, `read_max_ns` is about 1.9 µs. The 7–10 µs in 0.5.2 was measured with the trace on, which records every access on core 1.

### Known problems
- With the raw bus trace (`t`) on, one full-disk read stalled: the adapter set nIEN, waited 30 s, then reset the drive, and later dropped off USB. The trace slows core 0's sector fetches. Keep it off for data tests.
- `read_max_ns` (about 1.9 µs) is still over the slice 1 limit of 800 ns.

## [0.5.2] - 2026-10-23

### Fixed
- **An 18 µs stall on core 1 at every hardware reset.** Restoring the default geometry recomputed the IDENTIFY checksum over all 512 bytes, from flash. It now adjusts the checksum for the five changed words, in RAM. INITIALIZE DEVICE PARAMETERS benefits too.
- **The PIO program restarts when the host releases RESET#**, with empty FIFOs and DD and IORDY released. When the host is unplugged or powering up, its strobes float low. A write-path sample with DIOR# also low got an answer the PIO never pulled, which would leave every later read one answer behind.

### Changed
- Console `s`: `loop_max_ns` is now `read_max_ns`, the longest a read waited for its answer. It is measured from the last time core 1 saw the FIFO empty. Console `r` resets it.

### Known problems
- `read_max_ns` reports 7–10 µs on the rig, mostly during the adapter's register probe, although no read was answered after its strobe ended (0 in about 23,000). Not yet explained; a logic analyser on DIOR#, IORDY and CS0# should settle it.
- After the NS1081 puts the drive in standby and wakes it (IDLE IMMEDIATE), it fails every read without touching the bus, until it is replugged. The drive answers both commands with status 50.

## [0.5.1] - 2026-10-23

First run on hardware: the NS1081 adapter detects the drive build, and macOS sees the 64 MB disk.

### Fixed
- **Core 1 was too slow to answer reads on hardware** (up to 37 µs, against a budget of about 1 µs). The adapter gave up on reads, and the bus program fell out of step with the bus.
  - Core 1's per-access code ran from flash, and core 0 kept evicting it from the shared cache. It now runs from RAM (`CORE1_RAM`, `src/core1_ram.h`).
  - Arduino-Pico's periodic SysTick interrupt on core 1 is turned off.
  - Core 1 now checks the bus FIFO between each step of its loop, not once per pass.
  - `loop_max_ns` now reports the longest gap between those checks, which is how long a read can wait to be seen: about 650 ns (it was a whole pass).

### Changed
- Docs: the adapter is USB-powered (its brick only powers a drive), so the power-order rule is about its USB cable. `uhubctl` doesn't cut VBUS on the rig's hub, so only a physical replug restarts the adapter's IDE side.

## [0.5.0] - 2026-10-22

### Added
- **Activity classification (drive build), for the HDD Synth to choose a sequential or a random sound.**
  - Each media command is sequential (it carries on one of 4 tracked streams) or a seek, with its distance. The state comes from the share of seeks over a 250 ms window, with hysteresis and a 100 ms dwell.
  - The first command of a burst is neutral, and the drive wakes as Sequential and needs 3 commands to call Random.
  - Idle comes 150 ms after the last command ends.
  - Printed as `ACT` lines on each change; `s` shows the totals and the state.
  - See `docs/activity.md`.
- Console `m` prints a `MARK` timestamp, so tests can line phases up with the trace.
- `test/tools/classify_trace`: replays a recorded trace through the classifier, for tuning.
- Rig test `-m activity`: sequential, idle, random and interleaved phases from the Mac, each at least 90% in the right state.

## [0.4.0] - 2026-10-22

### Added
- A solder-bridge test, run from the console in both builds with the IDE cable unplugged: `x` explains it, then `Y` within 10 s runs it.
  - Every bus pin and its header neighbours (GPIO 0–3, 31) are pulled up; each bus pin in turn is driven low for 50 µs.
  - It reports `STUCK` (low with nothing driven: shorted to GND or the LED) and `SHORT` pairs, then reboots.
  - Core 1 is paused while it runs.
  - The analysis (`short_test.*`) is host-tested against simulated boards with known faults.
- `docs/bringup.md`: the checklist for hardware day, from continuity checks to slice 3.

## [0.3.0] - 2026-10-22

### Added
- The housekeeping commands that USB bridges and BIOSes send while setting a drive up, pulled forward from slice 6 so a bridge doesn't give up on the drive during setup:
  - SET FEATURES "set transfer mode": accepts PIO default (00h) and PIO flow-control mode 0 (08h); aborts DMA, faster PIO, IORDY off and the other subcommands;
  - INITIALIZE DEVICE PARAMETERS: sets the CHS translation used by CHS reads and reported in IDENTIFY words 54–58 (checksum updated); kept across a soft reset, reset by a hard one;
  - READ VERIFY (range check, no data), SEEK (70h–7Fh) and RECALIBRATE (10h–1Fh);
  - STANDBY, IDLE (and their immediate forms) and CHECK POWER MODE, in both the current and the ATA-1 opcodes;
  - FLUSH CACHE.
- SET MULTIPLE MODE is explicitly aborted, since IDENTIFY offers no multiple mode.
- A virtual-host test of a bridge's setup sequence.

## [0.2.0] - 2026-10-22

### Added
- **The drive build (`bb48_drive`)**, for slices 1–3. Core 1 serves the bus with `ide_bus` and runs the ATA device. Core 0 fetches sectors from the generated 64 MB disk through a cross-core mailbox, and never blocks on USB serial.
  - It traces commands as `CMD`/`DONE` lines, with LBA, count, status, error and duration.
  - Console `t` adds raw register accesses; `r` resets the loop-time maximum.
  - `s` now reports `loop_max_ns`: core 1's longest loop pass, measured with the M33 cycle counter.
- READ SECTORS (and its no-retry alias) in `ata_device`:
  - core 1 shows BSY while core 0 fetches each sector, then one DRQ block and one interrupt per sector;
  - a sector count of 0 means 256;
  - LBA and default-geometry CHS addressing;
  - IDNF for out-of-range addresses, before any data is sent;
  - UNC when a fetch fails;
  - stale fetches after a reset are ignored.
- `BlockSource` with a `GeneratedDisk` backend, and `FetchMailbox`, the core 1 ↔ core 0 handoff. A two-thread test is clean under ThreadSanitizer.
- Rig tests for slice 1 (gets past detection, plus core 1's loop time). The slice 2 and 3 tests now check the drive's trace as well as the Mac's view, and every rig test skips unless the board runs the build it needs.
- `scripts/rig.sh` takes `PIO_ENV` to choose the build. CI builds both, and releases carry both UF2s.

### Added (no firmware change)
- `ata_device`: the drive's register file and command state machine for slices 1 and 2. It covers:
  - status and interrupts;
  - hard and soft reset with the ATA signature;
  - answering for the absent device 1;
  - IDENTIFY DEVICE and EXECUTE DEVICE DIAGNOSTIC;
  - aborting everything else.
  It is tested against a virtual host's start-up probe. Not yet on the bus.
- `ide_bus.pio`: the slice 1 bus program. It pulls IORDY low as soon as a chip select is asserted, answers reads with values from core 1, and captures writes. A co-simulation with a mode 0 host checks every timing it must meet. Built but not yet run by the firmware.
- `gen_disk`: the slice 3 test disk, computed from the LBA, in C++ and Python with shared known answers.
- Rig tests for slice 2 (capacity and IDENTIFY over repeated replugs) and slice 3 (full-disk and random reads through the raw device).
- GitHub Actions: builds the firmware and runs the no-hardware tests on every push; a `vX.Y.Z` tag publishes a release with the UF2.

## [0.1.0] - 2026-10-22

### Added
- Slice 0, listen only: a PIO program captures every IDE register access (DIOR#/DIOW#) without driving any pin.
- Core 1 timestamps each access and every RESET# edge into a trace ring; core 0 merges repeated accesses and prints the trace over USB serial.
- Serial console: `h` help, `p` pin levels, `s` stats, `b` reboot to bootloader.
- IDENTIFY DEVICE block builder for slice 2 (not yet served on the bus).
- Host tests:
  - C++ unit tests;
  - a PIO simulator that runs the real program against ATA mode 0 timings;
  - the slice 0 rig test with a manual or uhubctl replug.
