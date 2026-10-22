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

- **Build:** `pio run` (PlatformIO lives in `~/.platformio-venv/bin/pio` on the dev Mac). The board is `olimex_pico2bb48`. There are two builds, selected by `FIRMWARE_SLICE`:
  - `bb48` (0): listen only;
  - `bb48_drive` (3): the drive, slices 1–3.

  `main.cpp` holds both. Rig tests skip unless the board runs the build they need, and `scripts/rig.sh` takes `PIO_ENV`.
  - The variant defines `PIN_LED` as a macro, so don't reuse Arduino pin-macro names for constants.
- **PIO programs:** the platform assembles each `src/*.pio` into a `.pio.h` next to it (git-ignored) before every build. Include them as `"name.pio.h"`.
- **Host unit tests:** `test/run_native_tests.sh` (plain `c++` with ASan/UBSan). Keep `bus_sample`, `trace_format`, `spsc_ring`, `identify`, `ata_device`, `gen_disk`, `block_source` and `fetch_mailbox` free of Arduino and SDK headers so they stay testable here. When touching the ring, the mailbox or the fetch handshake, also run `SANITIZE=thread test/run_native_tests.sh`.
- **CI:** `.github/workflows/build.yml` builds the firmware and runs the C++ tests and `pytest -m sim` on every push. A `vX.Y.Z` tag that matches `FIRMWARE_VERSION` publishes a release with the UF2.
- **Python tests:** in `host/` (pytest). One marker per slice; `sim` needs no hardware.
  - Plain `pytest` runs only the `sim` tests.
  - `host/hddproto/pio_sim.py` simulates the PIO instructions the programs use and runs the real `pioasm` output. Extend it when a program uses a new instruction, rather than letting it guess.
  - `host/hddproto/ide_host_model.py` is a mode 0 host that honours IORDY. `tests/test_pio_bus_sim.py` uses it to hold `ide_bus.pio` to the ATA timings, and the sim must be configured as `ide_bus.cpp` configures the state machine.
- **On the rig:** `scripts/rig.sh -m sliceN` runs the unit tests, builds, flashes and runs the rig tests.
  - Evidence goes to `host/runs/` (git-ignored); reference traces live in `host/reference/`.
- **Rig:**
  - The adapter is a Norelsys NS1081 (USB `2537:1081`, Bulk-Only, two units `1081CS0`/`1081CS1`). Its logic is powered by the brick, not USB.
  - It splits the ATA model at the first space into SCSI vendor/product, so match disks on diskutil's `IORegistryEntryName`, not `MediaName`.
  - A real CF card works through it.
  - It sits on port 4 of a Realtek RTS5411 hub that `uhubctl` controls; both USB 3 and USB 2 halves must be switched, and macOS needs `-e`.
  - `host/rig.env` (git-ignored, from `rig.env.example`) holds the replug settings, and `scripts/rig.sh` loads it.
- **Host OS:** the dev machine is macOS. There's no `O_DIRECT` (use `/dev/rdiskN`), no `dmesg` (use `log show`), and no ATA pass-through. OS-specific code stays in `host/hddproto/host_macos.py`; slice 6 adds a Linux host for pass-through.

## Drive data path

Core 1 owns `ata::Device` and never waits. For a read:
1. The device sets BSY and exposes a `Fetch` (id, LBA, its sector buffer).
2. Core 1 posts it once to `FetchMailbox`.
3. Core 0 fills the buffer from a `BlockSource` and posts the result.
4. Core 1 calls `fetchDone()`, which sets DRQ, unless the id is stale after a reset.

The device doesn't touch the buffer while BSY. Core 0 must never block for long: it serves fetches before and between trace prints, and only prints when USB serial has room. The SD card (slice 5) becomes another `BlockSource`.

## Trace format

Core 1 records events into an SPSC ring, and core 0 prints them (`src/trace_format.h`).
- Identical consecutive accesses are merged into one `EV` line with a count.
- The drive build logs commands as `CMD`/`DONE` lines; raw accesses only after `t`.

The Python parser is `host/hddproto/trace.py`; change both together.

Console:
- `h`: help, and the READY line with version and build;
- `p`: pin levels (wiring check);
- `s`: stats, including `loop_max_ns` on the drive build;
- `r`: reset the loop maximum;
- `t`: raw bus trace on or off;
- `b`: reboot to the bootloader.

## Versioning

SemVer in `src/version.h` (`FIRMWARE_VERSION`, plus `FIRMWARE_SLICE`). Bump it with any firmware change, add a `CHANGELOG.md` entry (Keep a Changelog), and tag `vX.Y.Z`.

## Status

- **Slice 0 (listen only):** written and tested on the Mac (unit tests, PIO simulation, a clean build). It is the running firmware. Waiting for the board.
- **Slices 1–3: the `bb48_drive` build**, built and unit-tested, not yet run on hardware. It contains:
  - `ide_bus.pio`, the bus program, with its timing proven in simulation: core 1 must answer a read within about 1,070 ns, and the slice 1 test checks `loop_max_ns` < 800;
  - `ata_device`: IDENTIFY, READ SECTORS, DIAGNOSTIC, and the housekeeping commands bridges send during setup (SET FEATURES transfer mode, INITIALIZE DEVICE PARAMETERS, READ VERIFY, SEEK, RECALIBRATE, power commands, FLUSH CACHE). Everything else gets ABRT;
  - the generated disk, served through the mailbox.
- **Next:**
  1. Wire the board and run `scripts/rig.sh -m slice0 --save-reference`.
  2. Add the adapter's probe sequence as a virtual-host test.
  3. Run `PIO_ENV=bb48_drive scripts/rig.sh -m slice1`, then slices 2 and 3.
  4. WRITE SECTORS and the RAM disk (slice 4) reuse the same handshake in the other direction.
