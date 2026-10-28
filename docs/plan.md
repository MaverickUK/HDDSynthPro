# Pico IDE Drive Emulator: Prototype Plan

22 Oct 2026, Peter Bridger. Amended the same day after checking the plan against the BB48 design files and this Mac; the amendments are marked **(amended)**.

## Goal and scope

The prototype proves one thing: an RP2350B can act as an IDE hard disk that a real host reads and writes reliably. Speed, features and the HDD Synth side all come later, in thin layers on top of a working base.

**In scope**
- One device, configured as master, no slave on the cable.
- PIO mode 0 only, with IORDY used to buy time.
- LBA28 addressing. The IDENTIFY block reports a default CHS geometry from slice 2 onwards, because some hosts read it. CHS commands come in slice 6. **(amended)**
- A small fixed-size disk: first generated or in RAM, then a contiguous image file on SD.
- The core command set: IDENTIFY DEVICE, READ SECTORS, WRITE SECTORS, and the handful of housekeeping commands hosts send.

**Out of scope for now**
- Every HDD Synth feature (audio, sample packs, encoder, OLED, supercap).
- Faster PIO modes, READ/WRITE MULTIPLE, any kind of DMA.
- ATAPI, slave operation, cable select, 44-pin hosts and real retro machines.
- Bus transceivers and the final board.

**Done means:** the USB-IDE adapter enumerates the drive; the host can partition, format, mount, write and read it; and an overnight soak test of random reads and writes finishes with zero mismatches.

## Test rig **(amended)**

The Mac that builds the firmware also flashes it, talks to the drive through the USB-IDE adapter, and reads the drive's own log over USB serial. Slice 6 adds a Linux machine (a Raspberry Pi 5) for ATA pass-through, which macOS doesn't offer for USB storage.

| Part | Role |
|---|---|
| Olimex RP2350-PICO2-BB48 on a Perma-Proto board | The emulated drive |
| IDC breakout + IDE ribbon | Host connection, wired per [wiring.md](wiring.md) |
| Adafruit SPI/SDIO microSD breakout + five 10 kΩ pull-ups | Image storage from slice 5 (GPIO 34–39; see [wiring-sd.html](wiring-sd.html)) |
| USB-IDE adapter (Norelsys NS1081; USB-powered, its brick only powers a real drive) | The host under test |
| Mac (slices 0–5), Raspberry Pi 5 (slice 6) | Test runner, flashing, log capture |
| Spare Pico 2 as a logic analyser (sigrok) | Bus traces when a test fails |

**Pin map (amended):**

| Lines | GPIO |
|---|---|
| DD0–15 | 4–19 |
| DA0–2 | 20–22 |
| CS0#, CS1# | 23, 24 |
| DIOR#, DIOW# | 26, 27 |
| IORDY | 28 |
| INTRQ | 29 |
| RESET# | 30 |

The original map put DD2/DD3 on GPIO 2/3, which the BB48 pulls up to 3.3 V for I2C. The new map also leaves out GPIO 25, the board's LED.

**Power-order rule:** the BB48 is powered before the adapter's USB is plugged in, and the adapter is unplugged before the BB48. There are no buffers, and the RP2350's pins are only 5 V tolerant while it is powered.

**Worth adding if automation gets annoying:**
- ~~A USB hub with per-port power switching that `uhubctl` supports~~: the rig's Realtek hub already does this (see below).
- A USB hub that really switches VBUS (or a USB power switch), for hands-free cold starts of the adapter.
- A Raspberry Pi Debug Probe for SWD.

## Automation loop

`scripts/rig.sh -m sliceN` runs the host unit tests, then builds, flashes and runs the rig tests. `PIO_ENV` picks the firmware: `bb48` (listen only) for slice 0, `bb48_drive` for slice 1 onwards. Every code change gets the full suite with no hands on the rig (once a switchable hub is fitted). A failing run leaves its evidence in `host/runs/`.

- **Flash:** `pio run -t upload` resets the board into the bootloader over serial and loads it with picotool. The runner then waits for the `READY` line. USB power stays on throughout.
- **Replug:** if a hub is configured, `uhubctl` power-cycles the adapter's port; otherwise the test asks for a manual replug and waits for bus activity.
- **Tests:** pytest, one marker per slice, so earlier slices re-run as regression tests. The `sim` marker covers the tests that need no hardware.

**Two views of every test.**
- **Host view:** what macOS sees: the system log, `diskutil`, and data read and written through the raw device `/dev/rdiskN`, which bypasses the cache (macOS has no `O_DIRECT`).
  - macOS mounts and indexes new disks, so traces include its reads too.
  - **(amended)**
- **Drive view:** the trace streamed over USB serial. A test passes only when both views agree.

## Firmware shape

Arduino-Pico (earlephilhower core, which runs on the Pico SDK) under PlatformIO, as in HDDSynth_Native. The platform assembles the PIO programs (`src/*.pio`) at build time.

| Module | Job | Testable on the Mac? |
|---|---|---|
| `ide_listen` (slice 0), `ide_bus` (slice 1 on: PIO + C++ wrapper) | Sees strobes, captures register accesses, drives reads, controls IORDY | In a simulator: `host/hddproto/pio_sim.py` runs the real assembled program, and `ide_host_model.py` plays a mode 0 host against it |
| `ata_device` | Task-file registers, status bits, the command state machine | Yes |
| `identify` | The IDENTIFY DEVICE block | Yes |
| `block_source` | Where sectors come from: `GeneratedDisk` now, then RamDisk and SdImage | Yes, with a fake SD |
| `fetch_mailbox` | Hands sector fetches from core 1 to core 0 and back | Yes, including a two-thread test |
| `trace_format`, `spsc_ring` | Trace events, their text form, the core 1 → core 0 ring | Yes |
| `main` | Console and core wiring | No |

**Core split:**
- Core 1 runs the IDE side only, in a tight loop that never touches the SD card or USB.
- Core 0 owns USB serial, the SD card and the trace output.

### The slowest-safe bus approach

**The deadline (amended):** the strict limit is **tA = 35 ns**. That is the time from DIOR#/DIOW# going low until IORDY must be low. It is tighter than the ~115 ns data deadline, and leaves about 5 PIO cycles at 150 MHz.

The answer is to trigger on the chip-selects instead of the strobe. CS0#/CS1# and DA0–2 are valid at least 70 ns (t1) before the strobe falls, so PIO can pull IORDY low as soon as a chip-select goes active. That leaves about 105 ns. Any deglitch filter belongs on the chip-selects, not the strobes.

The read path:
1. A chip-select goes active. PIO pulls IORDY low and hands the register address to core 1.
2. Core 1 looks up the value and pushes it back.
3. PIO drives the data bus once DIOR# is low, then releases IORDY. It tri-states the bus when DIOR# rises.

Simulated against a mode 0 host, `ide_bus.pio` measures:
- IORDY is low about 43 ns before the host samples it.
- The PIO's own share of a read is about 180 ns, so core 1 must answer within about 1,070 ns to keep IORDY under 1.25 µs.
- Read data is set up 63 ns before IORDY rises.
- DD is released 10 ns after DIOR# rises (the limit is 30 ns).

Host writes need no wait: PIO keeps sampling while DIOW# is low and keeps the last sample. That sample is taken 0–40 ns before DIOW# rises, inside mode 0's 60 ns data setup time. PIO modes 3/4 (setup 30/20 ns) will need a faster loop.

Rules:
- IORDY waits stay under 1.25 µs.
- Slow work never hides behind IORDY: anything that touches the SD card happens while STATUS shows BSY.

## Thin slices

Each slice ends in a passing automated test before the next starts.

- **Slice 0: listen only.**
  - Every pin is an input under SIO control, so PIO cannot drive it.
  - The trace shows every access and reset.
  - **Exit:** a clean, repeatable trace of the adapter's probe sequence, saved with `--save-reference`.
  - **(amended)** With no drive answering, the adapter probes forever and the trace dropped events. Since 0.10.2 the listen build records a window of 3,000 accesses after each RESET# (console `w`) and the test compares the probe's opening with a reference kept per adapter. Still to run with a physical replug.
- **Slice 1: answer status.** Done on hardware; reads are within ATA's limit since 0.6.3.
  - The register file, with status reads returning ready after reset, and soft reset through Device Control.
  - This is the first use of the IORDY read path.
  - **Exit:** the adapter gets past detection on 20 replugs in a row. **(amended)** And IORDY never holds a read past ATA's 1250 ns (tB), measured by a second PIO program (`iordy_timer.pio`) rather than estimated from core 1's loop time.
- **Slice 2: IDENTIFY DEVICE.** Done on hardware.
  - Model `HDDSYNTH PROTO`, LBA28 capacity, default CHS, PIO 0 only, IORDY, no DMA, no MULTIPLE.
  - The trace must show the same IDENTIFY sequence as the saved reference. Adapters can retry, and macOS can add commands, so this checks against the reference rather than for exactly one IDENTIFY. **(amended)**
  - **Exit:** a disk of the right capacity appears on 20 replugs out of 20.
- **Slice 3: read from a generated disk.** Done on hardware.
  - READ SECTORS from a disk whose content is computed from the LBA (64 MB). Sector count 0 means 256, and reads past the end fail cleanly.
  - **Exit:** a full-disk read and random reads with zero mismatches.
- **Slice 4: write to a RAM disk.** Done in 0.6.0.
  - WRITE SECTORS into a 256 KB RAM disk. **(amended)** It is an overlay on the 64 MB generated disk, so the disk keeps its size and slice 3 still applies; the bus program holds IORDY when a write burst outruns core 1.
  - **Exit:** 1,000 write-then-read cycles clean.
- **Slice 5: image file on SD.** Done in 0.7.0.
  - FatFs in SPI mode, a contiguous image, every SD access under BSY. **(amended)** SdFat (bundled with Arduino-Pico) instead of FatFs; the card is on GPIO 34–39 so the same wiring later runs 4-bit SDIO.
  - **Exit:** the full file-system cycle passes and the image survives a power cycle.
- **Slice 6: housekeeping commands and soak.** The housekeeping commands were pulled forward into 0.3.0, so a bridge's setup can't stall slice 2. They're unit-tested and still need checking against a real host. Slice 6 keeps the soak and the pass-through tests. A 1-hour soak passed on 0.7.0 (2.3 GB, 79,309 commands, no mismatches, worst read 953 ns). The overnight run on 0.7.1 found one transient mismatch in 8 hours (and an earlier one in an hour's run), both the first sector of the second command when a long read is split at 128 sectors. On 0.8.22, with the bus observer: 1 hour clean until a macOS USB event reset the adapter, then 4.5 hours clean (13.9 GB, 479,562 commands, 23.8 million sectors checked on the cable). Then 8 hours clean from Windows (26 Oct; 380 minutes until Windows Update restarted the PC, then the last 100: 22.1 GB, 760,622 commands, 37.8 million sectors checked on the cable, worst read 1,120 ns). That makes 13.5 clean hours on 0.8.22, which puts the old fault's chance of still being there at its old rate near 1%. Windows never sent STANDBY in the idle pauses, so those 8 hours didn't test standby and wake. On 0.8.24, whose core 1 code is the same, another 8 hours from Windows with write-behind on (27 Oct; 22.1 GB, 760,100 commands, 37.9 million sectors checked on the cable, worst read 1,173 ns): 21.5 clean hours in all.
  - CHS addressing, INITIALIZE DEVICE PARAMETERS, SET FEATURES, READ VERIFY, SEEK, RECALIBRATE, EXECUTE DEVICE DIAGNOSTIC, IDLE, STANDBY, CHECK POWER MODE, FLUSH CACHE. Anything unknown gets ABRT.
  - Pass-through tests run from the Pi 5.
  - **Exit:** an overnight soak with zero mismatches and zero hangs.

**After slice 3: activity classification.** Can the drive tell sequential from random activity, for the HDD Synth's sounds? The classifier and its tests are done; `-m activity` on the rig proves it on the generated disk. Real workloads (file copies, OS boots) can follow now that slice 5 gives it a file system. See [activity.md](activity.md).

## Later layers

1. Faster register path (status from PIO without IORDY; DMA-fed sector data).
2. READ/WRITE MULTIPLE.
3. PIO modes 1–4.
4. SDIO 4-bit.
5. Image selection and VHD footers.
6. A 486-class host.
7. Slave and cable select.
8. Amiga A600/A1200.
9. An adapter PCB with transceivers: targets and a draft BOM in [pcb-bom.md](pcb-bom.md).
10. HDD Synth integration.
11. An ATAPI CD-ROM drive serving a data ISO: a first exploration, planned in [atapi.md](atapi.md).
12. Later still: Iomega ZIP and LS-120 drives (also ATAPI, removable and writable), with their own sounds on the HDD Synth: see [atapi.md](atapi.md#a-future-idea-iomega-zip-and-ls-120).

## Risks and open questions

| Risk | Mitigation |
|---|---|
| Strobe ringing on hand-wired links | 33 Ω series resistors on DIOR#/DIOW#; a filter on the chip-selects, not the strobes (tA) |
| The adapter is fussy | The slice 0 trace shows what it does; keep a second adapter with a different chipset |
| No ATA pass-through (always the case on macOS) | Slice 6 pass-through tests on a Pi 5 |
| Wrong power order | The rule above; a relay-controlled brick later |
| Erratum E9 on A2-stepping RP2350s | Never use internal pull-downs |
| SD write stalls | BSY covers every SD access; measure the worst case in slice 5 |

**Answered (22 Oct 2026, adapter plugged into the Mac):**
- **The adapter is a Norelsys NS1081** (USB `2537:1081`): a USB 3.0 bridge with SATA and IDE ports.
  - macOS drives it with the standard Bulk-Only mass-storage driver (not UAS).
  - It exposes two units, `1081CS0` and `1081CS1`, presumably one per port.
  - The disk will probably be named after the bridge, not the drive's model, so the tests find it by size (or `HDDPRO_DISK`).
- **The IDE port works with a real drive.** A SanDisk 256 MB CompactFlash card (`SDCFJ-256`) on a CF-to-IDE adapter appeared as a 501,760-sector disk and mounted.
  - The bridge passes the ATA model through, split at the first space into the SCSI vendor and product: "SanDisk" / "SDCFJ-256".
  - Our "HDDSYNTH PROTO" will arrive as "HDDSYNTH" / "PROTO", so `find_disk` matches on the full IORegistry name ("HDDSYNTH PROTO Media"), not diskutil's `MediaName`.
- **The adapter is powered by USB**; its brick only powers a drive plugged into it. Its LED stays lit with the port switched off by `uhubctl`, because the hub doesn't cut VBUS.
- **Hands-free replugs only re-enumerate.** The adapter sits on port 4 of a Realtek RTS5411 USB 3 hub (`0bda:0411`, with a USB 2 half `0bda:5411`), which `uhubctl` controls.
  - Switching both halves of the port off removes the adapter from USB within 2 s, and it re-enumerates within about 1 s of switching on.
  - The settings are in `host/rig.env` (see `host/rig.env.example`).
  - The adapter keeps power, so its IDE side doesn't restart: no RESET#, no new IDENTIFY. A physical replug does both (23 Oct 2026).

**Open questions:**
- ~~Does a USB replug make the adapter probe the IDE bus again?~~ A physical one does; a `uhubctl` one doesn't (see above).
- ~~Will the NS1081 accept a PIO-only drive?~~ Yes (23 Oct 2026): it sends IDENTIFY, SET FEATURES with transfer mode `0c` (rejected) then `08` (PIO mode 0, accepted), and reads with READ SECTORS.
- Is the BB48's RP2350 the A2 or A4 stepping?
