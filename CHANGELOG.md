# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

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
