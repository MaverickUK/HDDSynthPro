# CLAUDE.md: context for working on HDDSynthPro

HDDSynthPro will become an enhanced HDD Synth that emulates the IDE drive itself instead of listening to one. It starts as a proof of concept: an Olimex RP2350-PICO2-BB48 (RP2350B) acting as an IDE hard disk behind a USB-IDE adapter. The plan, in thin slices, is in [docs/plan.md](docs/plan.md); the wiring is in [docs/wiring.md](docs/wiring.md).

It follows the approach of [HDDSynth_Native](https://github.com/MaverickUK/HDDSynth_Native): Arduino-Pico under PlatformIO, with core 0 for USB/SD and core 1 for the real-time work. HDD Synth code merges in later; keep the synth out of this repo for now.

The owner is an experienced software architect (C#/.NET, TypeScript, Python) who is new to embedded C++. Explain embedded-specific decisions briefly, and favour clear, idiomatic code over clever code.

## Rules that keep the bus safe

- **Pins:** the pin map in `src/config.h`, `docs/wiring.md` and the tables in `docs/wiring.html` (IDE) and `docs/wiring-sd.html` (microSD), the interactive diagrams, must agree.
  - GPIO 0–2 stay off the bus: the BB48 pulls GPIO 2/3 up to 3.3 V (R17/R18). GPIO 3 is DMACK# (an input; the host drives it) and GPIO 31 DMARQ, wired on the rig since 0.16.2 and on the PCB.
  - The PCB (`hardware/`, the RP2350B board in `docs/pcb.html`) keeps the same GPIOs; `hardware/tools/check_netlist.py` checks it against `src/config.h`.
  - GPIO 25 is the LED: the drive build's activity light (core 0's `updateLed()`, `cfg::LED_MIN_MS`).
  - GPIO 0 is the activity link's UART0 TX (`cfg::PIN_LINK_TX`, docs/activity-link.md); GPIO 1 is reserved for its reply and stays untouched.
  - DD/DA/CS# must stay on consecutive pins (one PIO read).
  - PCB only (unconnected on the BB48): GPIO 40 SD card detect, GPIO 41 BUS_EN# (the bus switches stay off until the firmware drives it low, which core 1 does once `ide_bus` has set the bus pins up, 0.16.6). First day with the board: [docs/pcb-bringup.md](docs/pcb-bringup.md).
- **Never use internal pull-downs** (RP2350 erratum E9). The host provides the bus pulls.
- **IORDY is open drain:** only pull it low or release it, never drive it high. The host pulls it up to 5 V.
- **Slice 0 never drives a pin:** the bus pins stay under SIO control, so the PIO program cannot drive them even if it is wrong. Driving starts in slice 1, and only on DD (during reads), IORDY, INTRQ and, during a DMA command only, DMARQ (GPIO 31, `ide_dma`; its output is forced off otherwise).
- **DD0-15 belong to one PIO block at a time** (GPIO function select): `ide_bus`'s, except during a multiword DMA read burst, when `ide_dma` hands them to its own block and back after it.
- **Timing:** IORDY must go low within 35 ns (tA) of a strobe. Trigger on the chip-selects, which are valid 70 ns earlier (see the plan).
- **Core split:** core 1 runs the bus loop and never touches USB, SD or anything slow. Everything it runs per access is marked `CORE1_RAM`, because code in flash can stall it for tens of µs; `scripts/check_core1_ram.py` (run by `rig.sh` and CI) fails if `loop1` can reach flash. Share flags with core 1 as `SharedFlag`, not `std::atomic<bool>`, whose members GCC may call out of line from flash. Slow work happens while STATUS shows BSY, never behind IORDY. Core 0 must keep up too: work it does per bus event (like the IORDY timer's reports) has to be cheap and bounded per call, or read-aheads are late and core 1's PIO accesses are held up (0.8.24). Core 1's speed also depends on where its code sits in RAM: a change elsewhere can move the fast lane and change how many reads wait, so benchmark reads (`-m speed`, or 4 MB sequential) after any core 1 change.

## Toolchain

- **Build:** `pio run` (PlatformIO lives in `~/.platformio-venv/bin/pio` on the dev Mac, `~/.platformio/penv/Scripts/pio.exe` on Windows; `rig.sh` looks in both). The board is `olimex_pico2bb48`. There are two builds, selected by `FIRMWARE_SLICE`:
  - `bb48` (0): listen only;
  - `bb48_drive` (3): the drive, slices 1–5.

  `main.cpp` holds both. Rig tests skip unless the board runs the build they need, and `scripts/rig.sh` takes `PIO_ENV`.
  - The variant defines `PIN_LED` as a macro, so don't reuse Arduino pin-macro names for constants.
  - The clock is 200 MHz (`board_build.f_cpu`, since 0.14.0), and `setup()` keeps the peripheral clock (SPI, UART) on it. Anything timed in cycles must say which clock: `pio_sim.CLOCK_HZ` matches the firmware's, and C++ works cycle counts out from `clock_get_hz(clk_sys)`.
- **PIO programs:** the platform assembles each `src/*.pio` into a `.pio.h` next to it (git-ignored) before every build. Include them as `"name.pio.h"`.
- **Host unit tests:** `test/run_native_tests.sh` (plain `c++` with ASan/UBSan). Keep `bus_sample`, `trace_format`, `spsc_ring`, `identify`, `ata_device`, `gen_disk`, `block_source`, `fetch_mailbox`, `short_test`, `activity`, `activity_link`, `drive_config`, `sector_check`, `media_watch` and `welcome_disk` free of Arduino and SDK headers so they stay testable here. When touching the ring, the mailbox or the fetch handshake, also run `SANITIZE=thread test/run_native_tests.sh`.
- **CI:** `.github/workflows/build.yml` builds the firmware and runs the C++ tests and `pytest -m sim` on every push. A `vX.Y.Z` tag that matches `FIRMWARE_VERSION` publishes a release with the UF2.
- **Python tests:** in `host/` (pytest). One marker per slice; `sim` needs no hardware.
  - Plain `pytest` runs only the `sim` tests.
  - `host/hddproto/pio_sim.py` simulates the PIO instructions the programs use and runs the real `pioasm` output. Extend it when a program uses a new instruction, rather than letting it guess.
  - `host/hddproto/ide_host_model.py` is a PIO host (modes 0-4) that honours IORDY. `tests/test_pio_bus_sim.py` uses it to hold `ide_bus.pio` to modes 0-3's timings, and the sim must be configured as `ide_bus.cpp` configures the state machine.
  - `host/hddproto/mdma_host_model.py` is a multiword DMA host; `tests/test_dma_sim.py` holds `ide_dma.pio` (the DMA data phase, [docs/dma.md](docs/dma.md)) to MDMA modes 0-2's timings. `pio_sim.assemble(..., program=)` picks one program from a file that holds several.
  - After a `git stash`/checkout that rewrites a `.py` within the same second and at the same size, Python can keep running the old one from `__pycache__`: delete the caches if a result looks stale.
- **On the rig:** `scripts/rig.sh -m sliceN` runs the unit tests, builds, flashes and runs the rig tests.
  - Evidence goes to `host/runs/` (git-ignored); reference traces live in `host/reference/`.
- **Rig:**
  - The adapter is a Norelsys NS1081 (USB `2537:1081`, Bulk-Only, two units `1081CS0`/`1081CS1`). It is powered by USB; its brick only powers a drive plugged into it.
  - It splits the ATA model at the first space into SCSI vendor/product, so match disks on diskutil's `IORegistryEntryName`, not `MediaName`.
  - A real CF card works through it.
  - It sits on port 4 of a Realtek RTS5411 hub that `uhubctl` controls; both USB 3 and USB 2 halves must be switched, and macOS needs `-e`.
  - `uhubctl` makes it re-enumerate but doesn't cut VBUS, so its IDE side doesn't restart. A fresh power-up probe needs a physical replug (see `CHANGELOG.md`, 0.6.2). A re-detection doesn't (0.11.0): console `V` takes the drive off the bus, the host's next read fails, the adapter resets and probes, and the drive comes back at its first RESET#, so it's detected again (IDENTIFY, SET FEATURES) and the read is retried. About 30 s; the host keeps the disk. `HDDPRO_REPLUG=redetect` makes `rig.replug()` do this, so slices 1-2 run their 20 cycles unattended.
  - With no drive answering, it probes forever (about one access per µs), so the listen build drops events and the bus is never quiet; the listen build's capture window (`w`) keeps a window after each RESET#.
  - On the Windows PC the same adapter enumerates as a JMicron (USB `152D:2338`), not as the NS1081 it showed on the Mac; why isn't known. There it was plugged straight into the PC (AMD USB 3 root hub), where on the Mac it went through the powered Realtek hub with the BB48. Without a drive it pulses RESET# every 4.8 s (a double pulse first) and runs the register check (A5/5A/AA/55) about 420,000 times a second. After the drive switches disks (`T`, `J`, an image change) Windows can keep the old disk's partitions mounted and refuse raw writes inside them (`EBADF`): `Update-Disk -Number N` makes it re-read them, and `host_windows.open_raw` does that before opening for writing. After Windows drops the disk, `pnputil /restart-device` on its current instance (the serial part changes) brings it back without a replug. It sometimes gives up on a half-read block and writes the next command without a reset (fixed in 0.11.0: it left the drive BSY for good).
  - **A declining adapter (27 Oct 2026), cleared by restarting the PC,** which had run for days with it plugged straight in; replugging the adapter alone only helped for about 40 minutes. Over an afternoon of redetect cycles it got steadily worse: double detections (a second RESET# about 15 ms after SET FEATURES 03h/08h) in most cycles, the disk lost, and IDENTIFY names garbled ("HD P", "HDDSYN P", " PRO") although the bus observer saw every IDENTIFY right on the cable (0 errors in about 100). Its own USB serial number (`984220294922`) came through in about 30 variants, each with the low four bits of some characters cleared ('9', '4', '2' read as '0'): corruption inside the adapter, which the drive can't cause. After the restart, 24 cycles on v0.11.0 didn't decline: about 1 cycle in 4 to 8 has a double detection, and a garbled name is rare (1-2 in 24, cleared by a USB restart). If cycles start failing more and more, in every build, restart the PC before blaming the firmware; the powered hub is still untried on this PC.
  - In PIO it waits about 10 ms after each READ command before polling; later sectors of the same command are polled at once. In multiword DMA (0.16.2) there's no such wait: the drive's DMARQ starts the transfer, so a 64 KB read costs 0.11 ms plus the data.
  - It sends CHECK POWER MODE, then STANDBY if the drive says active. What triggers it isn't settled: once it came straight after macOS probed a new disk, and a 60 s idle didn't trigger it (macOS's `disksleep` is 10 minutes). Waking (IDLE IMMEDIATE) must show BSY before the interrupt, or the adapter hangs.
  - At detection it asks for PIO flow-control mode 4 (SET FEATURES 03h, 0Ch) whatever IDENTIFY offers, then the fastest mode word 64 offers (3: 0Bh), else mode 0 (08h); never 1 or 2. Since 0.15.0 it runs in mode 3. In PIO it reads about one data word per 1.1 µs whatever the mode (mode 3 gained reads 2%, writes 13%), so faster PIO modes can't speed reads up on it; DMA modes may. It never sends FLUSH CACHE, not even for `F_FULLFSYNC`, and ignores multiple mode.
  - After a read error (UNC) in the middle of a transfer it goes silent for about 60 s, until macOS times out and resets it; it then re-detects the drive, and macOS drops the disk if reads still fail. Seen when the SD card was pulled during reads (2026-10-25): the drive failed the first read in 346 ms and later ones in 67 µs, and stayed responsive. Since 0.13.0 a card put back is re-mounted if it's the same card with the image unchanged (`src/media_watch.*`: CID, the image's size, first sector and modify time); anything else prints `CARD changed` and isn't served, since the host's cache would corrupt it. On the rig (Windows, `-m card`): a swap while idle is invisible (the failed read restarts the card and is retried); pulled during reads, each read fails in about 170 µs and the adapter resets and re-detects every 21 s, retrying the read; with the card back within 20 s the retry succeeds and the host sees no error, after 90 s out it had seen four, and either way Windows kept the disk and read the same data.
  - It uses DMA with drives that support it. Its 44-pin header shares the bus with the 40-pin one, so the listen build on the 40-pin header can record a real drive on the 44-pin.
  - `host/rig.env` (git-ignored, from `rig.env.example`) holds the replug settings, and `scripts/rig.sh` loads it.
- **Host OS:** the rig runs from macOS or Windows. `host/hddproto/host.py` holds what's common (replug, `find_disk`, `pread`/`pwrite`, which Windows lacks) and `make_host()` picks `host_macos.py` or `host_windows.py`; OS-specific code stays in those two. Tests use `make_host` and `hddproto.host.pread`/`pwrite`, never `os.pread`. Slice 6 adds a Linux host for pass-through.
  - macOS: there's no `O_DIRECT` (use `/dev/rdiskN`), no `dmesg` (use `log show`), and no ATA pass-through.
  - Windows: `\\.\PhysicalDriveN` (uncached), found with `Get-Disk`; it needs an administrator terminal. Run `scripts/rig.sh` from Git Bash. pyserial doesn't report the USB product there, so `find_port` falls back to the only port with the board's vendor ID (Olimex 15BA, or Raspberry Pi 2E8A), or `HDDPRO_PORT=COMn`. uhubctl hardly works, so replugs are by hand. The native C++ tests need WSL (no ASan/TSan); `rig.sh` skips them without a `c++`. Windows never sends the USB disk STANDBY (not even with the disk timeout at 1 minute), so soaks from Windows don't test standby and wake. Pause Windows Update before a long soak: it restarted the PC 6 h into one.

## Drive data path

Core 1 owns `ata::Device` and never waits. For a read:
1. The device sets BSY and exposes a `Fetch` (id, LBA, its sector buffer).
2. Core 1 posts it once to `FetchMailbox`.
3. Core 0 fills the buffer from a `BlockSource` and posts the result.
4. Core 1 calls `fetchDone()`, which sets DRQ, unless the id is stale after a reset.

A write (WRITE SECTORS) runs the other way: DRQ first, the host writes 256 words into the buffer, then BSY and a `Fetch` with `store` set, which core 0 writes to the `BlockSource`.

The device doesn't touch the buffer while BSY. Core 0 must never block for long: it serves fetches before and between trace prints, and only prints when USB serial has room.

The disk is the image `HDDSYNTH.INI` names (`docs/settings.md`, `src/drive_config.*`; `HDDSYNTH.IMG` by default), or `HDDSYNTH.IMG` if that one can't be used, on the SD card (`ImageDisk` over SdFat, `src/sd_card.*`), else `RamOverlayDisk` (writes stay in RAM, up to 512 sectors, until reboot) over the welcome disk (`src/welcome_disk.*`: a FAT16 volume computed per sector, holding `README.TXT` from `welcome/README.TXT`, embedded by `scripts/embed_welcome.py` into the git-ignored `src/welcome_text.h` before each build, and the sample `HDDSYNTH.INI`), or over `GeneratedDisk`, the test disk, with `image = none` or console `T` (`disk=generated`; the bus observer checks data only there). Core 1 waits at boot for core 0 to choose it. Never format the SD card or touch files on it other than the image being served, the scratch `HDDBENCH.IMG` and `HDDSYNTH.INI` (which the owner allowed the firmware to create and edit, with console `C`/`E` only): it holds the owner's data. Benchmarks that write use `HDDBENCH.IMG` (`disk=bench`). Slices 3–4 and the soak skip on an image, since they write to it, and on the welcome disk: for them, `T` (no card needed; then replug the adapter), or `E image = none` and `!` serve the generated disk with the card in (0.12.0), and `E image = HDDSYNTH.IMG` and `!` put the image back.

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
- `s`: stats; on the drive build also `iordy_slow`/`iordy_max_ns`/`iordy_max_reg`/`iordy_max_at_us` (reads IORDY held over 400 ns, and the longest: how long, which register, when; measured by `iordy_timer.pio`) `iordy_early` (reads that started before IORDY was low; in PIO mode 3 most do, harmlessly) `iordy_ignored` (reads the host ended while IORDY was still low, so it didn't wait: should be 0; a `NOIORDY` line if not, `iordy_check.pio`), `bus_sectors`/`bus_bad`/`bus_gaps`/`bus_dma_words`/`bus_ring_max` (the bus observer's check of data sectors on the cable, against the generated disk; a bad one prints `BUSERR`; `bus_ring_max` is the most samples ever waiting for core 0, of its ring's 4,096: near that, `bus_gaps` follow) `bus_identify`/`bus_identify_bad` (IDENTIFY blocks checked on the cable against the drive's own; on an SD image only for 10 s after each RESET# release, since checking every word slows reads; a bad one prints `BUSERR t IDENTIFY ...`) and `bus_contested`/`bus_stalls` (fast peripheral bus contention), plus `write_behind`, `image_name`/`config`, `card`/`card_lost`/`card_back` (the card the image is on: `present`, `lost` after a failed read or write, `changed` if a different card or an edited image came back; `CARD lost|back|changed` lines on each change) (the image served, and the settings file as read at start-up). Every read held past 1,250 ns also prints `SLOWREAD t register ns` (the time is when core 0 collected the report), and a command running over 3 s prints `STUCK` (the last fetch core 0 served, and whether its result is still waiting for core 1);
- `r`: reset the read timings (they also restart when the host releases RESET#);
- `t`: raw bus trace on or off;
- `D` then a digit: debug, answer data reads that many × 250 ns late (`-m timing`; `D0` is off);
- `F`: debug, core 1's fast lane for data reads off or on (to compare);
- `A`: read-ahead off or on until a restart (`read_ahead` in `HDDSYNTH.INI`);
- `m`: print a `MARK` timestamp (used by the activity rig test);
- `S`: start the microSD card and report it, then `CONFIG` lines: the settings from `HDDSYNTH.INI`, the image served, why a configured image wasn't used, warnings (drive build; only reads);
- `C`: create `HDDSYNTH.INI` with the defaults if it isn't there; `E` then `key = value` and Enter: set one setting (`image`, `mode`, `dma`, `write_behind`, `read_ahead`, `activity_link`), keeping the rest of the file; `!`: restart, to apply them (keeps `J` or `T`);
- `I`: create `HDDSYNTH.IMG` (64 MB, zeros) on the card if it isn't there; reboot to use it;
- `K`: create `HDDBENCH.IMG`, the scratch image for benchmarks that write; `J`: restart serving it, or back (kept in a watchdog scratch register, so a power cycle always returns to `HDDSYNTH.IMG`); `T`: restart serving the test disk, card or no card, or back (the same register, so `T` and `J` undo each other);
- `L`: the activity link on or off until a restart (`activity_link` in `HDDSYNTH.INI`; `s` shows `link`, `link_sent` and `link_last`);
- `U`: the link's loopback test, with a jumper from GPIO 0 to GPIO 1 (`LOOPBACK pass|fail|no-data ...`; `-m link`);
- `V`: the drive leaves the bus (`ABSENT on`) until the adapter's next RESET#, a second `V`, or 120 s (`ABSENT off ...`); for re-detection without a replug (`HDDPRO_REPLUG=redetect`);
- `W`: write-behind on or off until a restart (on by default since 0.17.0; `write_behind` in `HDDSYNTH.INI`);
- `x` then `Y`: the solder-bridge test, IDE cable unplugged (`short_test.*`; the board reboots afterwards);
- `w` (listen build): the capture window on or off: each RESET# assert opens a window of `cfg::LISTEN_WINDOW_EVENTS` accesses, the rest are counted (`skipped=` in `s`) but not recorded (`-m slice0` uses it);
- `b`: reboot to the bootloader.

## Versioning

SemVer in `src/version.h` (`FIRMWARE_VERSION`, plus `FIRMWARE_SLICE`). Bump it with any firmware change, add a `CHANGELOG.md` entry (Keep a Changelog), and tag `vX.Y.Z`.

## Status (29 Oct 2026, 0.18.0)

- **What works on the rig** (Windows, the JMicron-ID adapter, the BB48 with DMARQ/DMACK# wired): a read/write disk in PIO modes 0-3 (the adapter takes 3) and multiword DMA mode 2 (offered with `mode = mdma2`, or by `auto` with `dma = on`), serving the SD image or the built-in disk, at 200 MHz. Detection and re-detection, standby and wake (seen from macOS), FLUSH CACHE, no card at boot, a card pulled and put back (0.13.0), the activity link sending. Slices 0-5, `-m card`, `-m speed` and the soak pass. History, with every measurement, is in `CHANGELOG.md` and `docs/speed-log.md`.
- **Speeds:** on the built-in disk, PIO mode 3 reads 1.36 MB/s and writes 1.79; multiword DMA 2 reads 5.14 MB/s (2.6 while the bus observer checks every DMA word, which costs core 0), 969 random 4 KB reads a second and writes 9.76 MB/s. The adapter paces PIO reads itself (about 1.1 µs a word, and 10 ms before polling each READ); DMA avoids both. On the SD image the card over SPI tops out near 1.8 MB/s for reads; writes are 1.65 MB/s with write-behind (the default since 0.17.0), 1.05 without.
- **Timing:** the worst read in traffic and detection is 1,030-1,100 ns (ATA allows 1,250); detection's old slow reads went with 200 MHz (0.14.0). `iordy_ignored` (0.16.5) is 0 through the adapter.
- **Soaks:** 10 hours clean on 0.16.0 in PIO mode 3 (28-29 Oct: 28.9 GB, 49.5 million sectors checked on the cable, 0 bad, worst read 1,060 ns) and on 0.12.0 in mode 0. The rare read mismatch of 0.7.1 (one sector per 8-9 hours, bits 1 read as 0) hasn't been seen since 0.8.22, over more than 40 hours; if it comes back, the bus observer says whether it was wrong on the cable. **DMA soaked 6 hours** (0.17.0, 29 Oct): 32.9 GB, no mismatches, 1.13 million DMA commands, no resets, worst read 1,150 ns. But the observer flagged 102 sectors, all LBA 65535 word 3 (0000 where ffff was due, all 16 lines rising: the bus settling later than tE; the data lines are on the default 4 mA slow-slew drive), and missed 94,071 words (its 1,024-word buffer at DMA's rate). All three fixed in 0.17.1: the DMA read program waits 20-25 ns after driving DD before watching DIOR#, whose unsynchronised input saw the 16 lines switching as the strobe ending; the ring is 16 KB; the count is 64-bit. Stronger DD drive (8 mA, fast slew) made the noise worse, so DD keeps the default. 0.17.1 soaked 10 hours in DMA (29-30 Oct): 51.9 GB, no mismatches, 1.79 million DMA commands, no resets, worst read 1,170 ns, 89.0 million sectors checked on the cable, 0 bad, 0 gaps, `bus_ring_max` 2,570/4,096.
- **Slice 0 (listen only):** passes with a physical replug on the Windows rig (28 Oct), against `host/reference/slice0_probe_jmicron.txt`. After power-up the adapter resets twice, then waits about 2.6 s before probing; the test has it unplugged first (with no drive it pulses RESET# every 4.8 s) and waits for that first access.
- **The `bb48_drive` build** contains:
  - `ide_bus.pio`, every register access, IORDY holding the host while core 1 answers (proven in simulation for PIO modes 0-3; core 1 has about 1,140 ns per read at 200 MHz);
  - `ide_dma.pio` and `ide_dma.cpp`, multiword DMA's data phase in a PIO block of its own, fed by a DMA channel ([docs/dma.md](docs/dma.md));
  - the diagnostics: `iordy_timer.pio` (slow reads), `iordy_check.pio` (hosts that don't wait for IORDY), `bus_observer.pio` (data and IDENTIFY checked on the cable, PIO and DMA);
  - `ata_device`: IDENTIFY, READ/WRITE SECTORS and DMA, DIAGNOSTIC and the housekeeping commands bridges send; everything else gets ABRT;
  - the disk through the mailbox: the SD image (`sd_card`, with `media_watch` for a card pulled and put back) or, with no card or image, the welcome disk (0.18.0: `README.TXT` and a sample `HDDSYNTH.INI` on FAT16; not yet tried on the rig), or the generated disk on request (`image = none`, console `T`), both with the RAM overlay; settings from `HDDSYNTH.INI` (`drive_config`).
- **Next:**
  1. DMA as the default (`drive_config::DEFAULT_DMA`): ready since 0.17.1's 10-hour DMA soak, but kept off until a retro PC has run in PIO first.
  2. The activity link: the loopback test with a jumper from GPIO 0 to GPIO 1 (console `U`, `-m link`), then the HDD Synth's side (`docs/hddsynth-activity-link-brief.md`; HDDSynth_Native v0.18.0 has it).
  3. The PCB (rev 0.3, ordered): bring-up per [docs/pcb-bringup.md](docs/pcb-bringup.md); 0.16.6 drives its BUS_EN#. SDIO for the card is designed ([docs/pio-allocation.md](docs/pio-allocation.md): the three PIO blocks shared by need, one build) but unreliable on the rig's hand-wired leads (29 Oct), so it's tested on the board first. A card put in SPI mode stays there until power-cycled.
  4. PCB rev 0.4 ([docs/pcb-rev04.md](docs/pcb-rev04.md)): DASP#, PDIAG#, CSEL and IOCS16#, PSRAM on GPIO 47, an SD power switch, DD series-resistor footprints, a debug header.
  5. Retro hosts ([docs/retro-test.md](docs/retro-test.md), the checklist): a 486 on the built-in disk first, watching `iordy_ignored` and the bus observer; then a scratch image and benchmarks. Hosts with no IORDY need the register table designed in [docs/no-iordy.md](docs/no-iordy.md) (3-4 days; build it if a retro PC shows `iordy_ignored` above 0); ISA cards needing IOCS16# need PCB rev 0.4.
  6. Smaller: the Vantec CB-ISATAU adapter when it arrives; a second card refused (`-m card` with `HDDPRO_SECOND_CARD=1`); the DMARQ test reading `absent` with the wire off; the bus observer skipping sectors when behind, so DMA tests on the built-in disk run at full speed; why handling the mailbox mid-block broke detection (0.8.17). Write-behind's old slow reads on the card are gone (0.16.5), so the logic-analyser plan for them is shelved.
  7. Later: PIO mode 4 (its write data can fall between the bus program's samples), Ultra DMA modes 0-2 (after SDIO), a native PATA host for slice 6 (a JMB363/368 card), ATAPI ([docs/atapi.md](docs/atapi.md)).
