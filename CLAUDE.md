# CLAUDE.md: context for working on HDDSynthPro

HDDSynthPro will become an enhanced HDD Synth that emulates the IDE drive itself instead of listening to one. It starts as a proof of concept: an Olimex RP2350-PICO2-BB48 (RP2350B) acting as an IDE hard disk behind a USB-IDE adapter. The plan, in thin slices, is in [docs/plan.md](docs/plan.md); the wiring is in [docs/wiring.md](docs/wiring.md).

It follows the approach of [HDDSynth_Native](https://github.com/MaverickUK/HDDSynth_Native): Arduino-Pico under PlatformIO, with core 0 for USB/SD and core 1 for the real-time work. HDD Synth code merges in later; keep the synth out of this repo for now.

The owner is an experienced software architect (C#/.NET, TypeScript, Python) who is new to embedded C++. Explain embedded-specific decisions briefly, and favour clear, idiomatic code over clever code.

## Rules that keep the bus safe

- **Pins:** the pin map in `src/config.h`, `docs/wiring.md` and the `IDE` table in `docs/wiring.html` (the interactive diagram) must agree.
  - GPIO 0–3 stay off the bus: the BB48 pulls GPIO 2/3 up to 3.3 V (R17/R18).
  - GPIO 25 is the LED.
  - DD/DA/CS# must stay on consecutive pins (one PIO read).
- **Never use internal pull-downs** (RP2350 erratum E9). The host provides the bus pulls.
- **IORDY is open drain:** only pull it low or release it, never drive it high. The host pulls it up to 5 V.
- **Slice 0 never drives a pin:** the bus pins stay under SIO control, so the PIO program cannot drive them even if it is wrong. Driving starts in slice 1, and only on DD (during reads), IORDY and INTRQ.
- **Timing:** IORDY must go low within 35 ns (tA) of a strobe. Trigger on the chip-selects, which are valid 70 ns earlier (see the plan).
- **Core split:** core 1 runs the bus loop and never touches USB, SD or anything slow. Slow work happens while STATUS shows BSY, never behind IORDY.

## Toolchain

- **Build:** `pio run` (PlatformIO lives in `~/.platformio-venv/bin/pio` on the dev Mac). The board is `olimex_pico2bb48`.
  - The variant defines `PIN_LED` as a macro, so don't reuse Arduino pin-macro names for constants.
- **PIO programs:** the platform assembles each `src/*.pio` into a `.pio.h` next to it (git-ignored) before every build. Include them as `"name.pio.h"`.
- **Host unit tests:** `test/run_native_tests.sh` (plain `c++` with ASan/UBSan). Keep `bus_sample`, `trace_format`, `spsc_ring`, `identify` and later `ata_device`/`block_device` free of Arduino and SDK headers so they stay testable here.
- **Python tests:** in `host/` (pytest). One marker per slice; `sim` needs no hardware.
  - `host/hddproto/pio_sim.py` simulates the PIO instructions the programs use and runs the real `pioasm` output. Extend it when a program uses a new instruction, rather than letting it guess.
- **On the rig:** `scripts/rig.sh -m sliceN` runs the unit tests, builds, flashes and runs the rig tests.
  - Evidence goes to `host/runs/` (git-ignored); reference traces live in `host/reference/`.
- **Host OS:** the dev machine is macOS. There's no `O_DIRECT` (use `/dev/rdiskN`), no `dmesg` (use `log show`), and no ATA pass-through. OS-specific code stays in `host/hddproto/host_macos.py`; slice 6 adds a Linux host for pass-through.

## Trace format

Core 1 records events into an SPSC ring, and core 0 prints them (`src/trace_format.h`). Identical consecutive accesses are merged into one `EV` line with a count. The Python parser is `host/hddproto/trace.py`; change both together.

Console: `h` help/READY, `p` pin levels (wiring check), `s` stats, `b` reboot to bootloader.

## Versioning

SemVer in `src/version.h` (`FIRMWARE_VERSION`, plus `FIRMWARE_SLICE`). Bump it with any firmware change, add a `CHANGELOG.md` entry (Keep a Changelog), and tag `vX.Y.Z`.

## Status

- **Slice 0 (listen only):** written and tested on the Mac (unit tests, PIO simulation, a clean build). Waiting for the board.
- **IDENTIFY block (slice 2):** written and unit-tested; not yet served on the bus.
- **Next:** wire the board, run `scripts/rig.sh -m slice0 --save-reference`, and read the adapter's probe sequence before designing slice 1's read path.
