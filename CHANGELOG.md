# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

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
