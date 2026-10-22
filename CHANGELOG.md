# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
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
