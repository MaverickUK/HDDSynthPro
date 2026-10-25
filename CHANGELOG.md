# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

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
