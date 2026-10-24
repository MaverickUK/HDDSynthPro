# CLAUDE.md: context for working on HDDSynthPro

HDDSynthPro will become an enhanced HDD Synth that emulates the IDE drive itself instead of listening to one. It starts as a proof of concept: an Olimex RP2350-PICO2-BB48 (RP2350B) acting as an IDE hard disk behind a USB-IDE adapter. The plan, in thin slices, is in [docs/plan.md](docs/plan.md); the wiring is in [docs/wiring.md](docs/wiring.md).

It follows the approach of [HDDSynth_Native](https://github.com/MaverickUK/HDDSynth_Native): Arduino-Pico under PlatformIO, with core 0 for USB/SD and core 1 for the real-time work. HDD Synth code merges in later; keep the synth out of this repo for now.

The owner is an experienced software architect (C#/.NET, TypeScript, Python) who is new to embedded C++. Explain embedded-specific decisions briefly, and favour clear, idiomatic code over clever code.

## Rules that keep the bus safe

- **Pins:** the pin map in `src/config.h`, `docs/wiring.md` and the tables in `docs/wiring.html` (IDE) and `docs/wiring-sd.html` (microSD), the interactive diagrams, must agree.
  - GPIO 0–3 stay off the bus: the BB48 pulls GPIO 2/3 up to 3.3 V (R17/R18).
  - GPIO 25 is the LED.
  - DD/DA/CS# must stay on consecutive pins (one PIO read).
- **Never use internal pull-downs** (RP2350 erratum E9). The host provides the bus pulls.
- **IORDY is open drain:** only pull it low or release it, never drive it high. The host pulls it up to 5 V.
- **Slice 0 never drives a pin:** the bus pins stay under SIO control, so the PIO program cannot drive them even if it is wrong. Driving starts in slice 1, and only on DD (during reads), IORDY and INTRQ.
- **Timing:** IORDY must go low within 35 ns (tA) of a strobe. Trigger on the chip-selects, which are valid 70 ns earlier (see the plan).
- **Core split:** core 1 runs the bus loop and never touches USB, SD or anything slow. Everything it runs per access is marked `CORE1_RAM`, because code in flash can stall it for tens of µs. Slow work happens while STATUS shows BSY, never behind IORDY.

## Toolchain

- **Build:** `pio run` (PlatformIO lives in `~/.platformio-venv/bin/pio` on the dev Mac). The board is `olimex_pico2bb48`. There are two builds, selected by `FIRMWARE_SLICE`:
  - `bb48` (0): listen only;
  - `bb48_drive` (3): the drive, slices 1–3.

  `main.cpp` holds both. Rig tests skip unless the board runs the build they need, and `scripts/rig.sh` takes `PIO_ENV`.
  - The variant defines `PIN_LED` as a macro, so don't reuse Arduino pin-macro names for constants.
- **PIO programs:** the platform assembles each `src/*.pio` into a `.pio.h` next to it (git-ignored) before every build. Include them as `"name.pio.h"`.
- **Host unit tests:** `test/run_native_tests.sh` (plain `c++` with ASan/UBSan). Keep `bus_sample`, `trace_format`, `spsc_ring`, `identify`, `ata_device`, `gen_disk`, `block_source`, `fetch_mailbox`, `short_test` and `activity` free of Arduino and SDK headers so they stay testable here. When touching the ring, the mailbox or the fetch handshake, also run `SANITIZE=thread test/run_native_tests.sh`.
- **CI:** `.github/workflows/build.yml` builds the firmware and runs the C++ tests and `pytest -m sim` on every push. A `vX.Y.Z` tag that matches `FIRMWARE_VERSION` publishes a release with the UF2.
- **Python tests:** in `host/` (pytest). One marker per slice; `sim` needs no hardware.
  - Plain `pytest` runs only the `sim` tests.
  - `host/hddproto/pio_sim.py` simulates the PIO instructions the programs use and runs the real `pioasm` output. Extend it when a program uses a new instruction, rather than letting it guess.
  - `host/hddproto/ide_host_model.py` is a mode 0 host that honours IORDY. `tests/test_pio_bus_sim.py` uses it to hold `ide_bus.pio` to the ATA timings, and the sim must be configured as `ide_bus.cpp` configures the state machine.
- **On the rig:** `scripts/rig.sh -m sliceN` runs the unit tests, builds, flashes and runs the rig tests.
  - Evidence goes to `host/runs/` (git-ignored); reference traces live in `host/reference/`.
- **Rig:**
  - The adapter is a Norelsys NS1081 (USB `2537:1081`, Bulk-Only, two units `1081CS0`/`1081CS1`). It is powered by USB; its brick only powers a drive plugged into it.
  - It splits the ATA model at the first space into SCSI vendor/product, so match disks on diskutil's `IORegistryEntryName`, not `MediaName`.
  - A real CF card works through it.
  - It sits on port 4 of a Realtek RTS5411 hub that `uhubctl` controls; both USB 3 and USB 2 halves must be switched, and macOS needs `-e`.
  - `uhubctl` makes it re-enumerate but doesn't cut VBUS, so its IDE side doesn't restart. A fresh probe (RESET# and IDENTIFY) needs a physical replug; nothing the drive does can trigger one (see `CHANGELOG.md`, 0.6.2).
  - With no drive answering, it probes forever (about one access per µs), so the listen build drops events and the bus is never quiet.
  - It waits about 10 ms after each READ command before polling; later sectors of the same command are polled at once.
  - After about 40 s idle it sends CHECK POWER MODE, then STANDBY if the drive says active. Waking (IDLE IMMEDIATE) must show BSY before the interrupt, or the adapter hangs.
  - It uses DMA with drives that support it. Its 44-pin header shares the bus with the 40-pin one, so the listen build on the 40-pin header can record a real drive on the 44-pin.
  - `host/rig.env` (git-ignored, from `rig.env.example`) holds the replug settings, and `scripts/rig.sh` loads it.
- **Host OS:** the dev machine is macOS. There's no `O_DIRECT` (use `/dev/rdiskN`), no `dmesg` (use `log show`), and no ATA pass-through. OS-specific code stays in `host/hddproto/host_macos.py`; slice 6 adds a Linux host for pass-through.

## Drive data path

Core 1 owns `ata::Device` and never waits. For a read:
1. The device sets BSY and exposes a `Fetch` (id, LBA, its sector buffer).
2. Core 1 posts it once to `FetchMailbox`.
3. Core 0 fills the buffer from a `BlockSource` and posts the result.
4. Core 1 calls `fetchDone()`, which sets DRQ, unless the id is stale after a reset.

A write (WRITE SECTORS) runs the other way: DRQ first, the host writes 256 words into the buffer, then BSY and a `Fetch` with `store` set, which core 0 writes to the `BlockSource`.

The device doesn't touch the buffer while BSY. Core 0 must never block for long: it serves fetches before and between trace prints, and only prints when USB serial has room.

The disk is `HDDSYNTH.IMG` on the SD card (`ImageDisk` over SdFat, `src/sd_card.*`) if it exists, else `RamOverlayDisk` over `GeneratedDisk` (writes stay in RAM, up to 512 sectors, until reboot). Core 1 waits at boot for core 0 to choose it. Never format the SD card or touch files on it other than `HDDSYNTH.IMG`: it holds the owner's data. Slices 3–4 and the soak skip on an image, since they write to it.

The bus program holds IORDY on a write while its RX FIFO is full, so a write burst can't outrun core 1. `ide_bus.pio` fills its PIO block's 32 instructions.

## Activity classification

`src/activity.*` (pure C++, `test_activity.cpp`) classifies media commands as sequential or seeks, and the recent mix as IDLE/SEQ/RAND, for the HDD Synth's sounds.
- It runs on core 0, fed from the CMD/DONE events as they leave the trace ring.
- It prints `ACT` lines on each change.
- Thresholds are in `activity::Config`.
- To tune on recorded traces: `.pio/native-tests/classify_trace < trace.txt`.
- Rationale and limits: `docs/activity.md`.

## Trace format

Core 1 records events into an SPSC ring, and core 0 prints them (`src/trace_format.h`).
- Identical consecutive accesses are merged into one `EV` line with a count.
- The drive build logs commands as `CMD`/`DONE` lines; raw accesses only after `t`.

The Python parser is `host/hddproto/trace.py`; change both together.

Console:
- `h`: help, and the READY line with version and build;
- `p`: pin levels (wiring check);
- `s`: stats; on the drive build also `iordy_slow`/`iordy_max_ns`/`iordy_max_reg`/`iordy_max_at_us` (reads IORDY held over 400 ns, and the longest: how long, which register, when; measured by `iordy_timer.pio`) and `bus_contested`/`bus_stalls` (fast peripheral bus contention);
- `r`: reset the read timings (they also restart when the host releases RESET#);
- `t`: raw bus trace on or off;
- `m`: print a `MARK` timestamp (used by the activity rig test);
- `S`: start the microSD card and report it (drive build; only reads);
- `I`: create `HDDSYNTH.IMG` (64 MB, zeros) on the card if it isn't there; reboot to use it;
- `x` then `Y`: the solder-bridge test, IDE cable unplugged (`short_test.*`; the board reboots afterwards);
- `b`: reboot to the bootloader.

## Versioning

SemVer in `src/version.h` (`FIRMWARE_VERSION`, plus `FIRMWARE_SLICE`). Bump it with any firmware change, add a `CHANGELOG.md` entry (Keep a Changelog), and tag `vX.Y.Z`.

## Status

- **Hardware (23 Oct 2026):** the board is wired and passes the solder-bridge test. On the drive build, the NS1081 sends IDENTIFY, SET FEATURES (PIO mode 0) and READ SECTORS, and macOS sees the 64 MB disk.
- **On hardware (0.7.0):** slices 2–5 and the activity test pass: a read/write PIO mode 0 disk, either 64 MB generated with a 256 KB RAM overlay, or an image file on the SD card that macOS formats and that survives a power cycle. Standby and wake work. Open: reads are held 0.4–1 µs typically; the worst, around detection, is about 1 µs since 0.6.3 (ATA allows 1.25 µs, which slice 1 checks); each READ waits about 10 ms before the adapter polls. Keep the raw trace (`t`) off for data tests: it slows core 0's fetches enough to stall the adapter.
- **Slice 0 (listen only):** runs on the board. Its rig test can't pass as written: the adapter never goes quiet without a drive.
- **Slices 1–3: the `bb48_drive` build**, built, unit-tested and detected on hardware; the rig tests have not been run yet. It contains:
  - `ide_bus.pio`, the bus program, with its timing proven in simulation: core 1 must answer a read within about 1,070 ns, and on hardware the slice 1 test checks that IORDY never holds a read past 1250 ns (`iordy_timer.pio`). `loop1()` serves the FIFO first and does one bookkeeping step at a time;
  - `ata_device`: IDENTIFY, READ SECTORS, DIAGNOSTIC, and the housekeeping commands bridges send during setup (SET FEATURES transfer mode, INITIALIZE DEVICE PARAMETERS, READ VERIFY, SEEK, RECALIBRATE, power commands, FLUSH CACHE). Everything else gets ABRT;
  - the generated disk, served through the mailbox.
- **Next, to make PIO solid** (DMA, faster modes and clocks wait until then):
  1. Confirm the worst read stays under 1.25 µs over more replugs (0.6.3: about 1 µs).
  2. Partition and format the disk from macOS (the 512-sector overlay may be too small for a format).
  3. An overnight soak of mixed reads and writes (slice 6's exit test), and repeated replugs.
  4. Add the adapter's probe sequence as a virtual-host test.
