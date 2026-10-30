# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

## [0.23.6] - 2026-10-30

With 0.23.5 eight data reads failed, all among IDENTIFY's first words, answered 1.3-3 µs late: core 1 left its tight loops for the main loop around the command (its bookkeeping, a timer read) and whenever the lane met a status poll.

### Changed
- `early_answers`: after starting a command, core 1 goes straight back to the tight bus loop; the command's bookkeeping waits until the bus is quiet. The bus loop runs the data lane itself when a block starts. The lane serves other accesses (status polls) inline and carries on, leaving only for a new command or the block's end. Some commands' `CMD` trace lines may be lost in this mode.

## [0.23.5] - 2026-10-30

With 0.23.4 only the first status read after the reset failed, but 12 of IDENTIFY's 256 data words were late. The PIIX drops the chip select between data words and gives each t1 80 ns and a strobe of 175-195 ns (`RDTIME`), so core 1 has about 200 ns.

### Changed
- The early lane and bus loop check the RX FIFO (up to 32 reads in a row, `rxWaiting`) between each housekeeping item (RESET#, fetch posts and results), not once a round, so a request waits a few cycles.
- `ide_bus.cpp` reads the FIFO's empty flag with its own always-inlined `rxEmpty()`; the SDK's had ended up out of line in flash.

## [0.23.4] - 2026-10-30

`c1_gap_ns` showed core 1's main loop taking about 660 ns a pass on average, and up to 880, even idle. That's about 150 cycles of calls and checks, not slow memory: a micro-benchmark (`c1_bench_*`, in `s`) puts PIO, RAM and SIO reads at about a cycle each from RAM code. A request could wait most of the PIIX's 400 ns before core 1 looked at it.

### Changed
- **`early_answers`: after an access, core 1 stays in a tight bus loop** (`ide_bus::serveBusEarly`). It serves each access inline, posts fetches and takes their results (so BSY ends while the host polls), and leaves when the bus has been quiet for `EARLY_BUS_ONLY_US`, a command is queued, a data-in block starts, or RESET# changes. Not while the raw trace is on.
- Console `D`'s delay waits on core 1's cycle counter, always inlined: the SDK's `busy_wait_at_least_cycles()` had ended up out of line in flash.

### Added
- `s`: `c1_pass_ns` (core 1's average loop pass), `c1_fifo_cyc` (its FIFO check, average/longest) and the micro-benchmark (diagnostics on this branch).

## [0.23.3] - 2026-10-30

On the Pentium the timing probe measured t1 = 110 ns and strobes of 335-345 ns in the PIIX's compatible timing. That's more relaxed than PIO mode 0's minimum, yet the drive answered as late as 490 ns into the strobe. So it's core 1's response time, not the host's speed.

### Added
- **`c1_gap_ns` in `s`:** core 1's longest main-loop pass, by what the pass did (an access, the fast lane, a command, bus-only, steps 0-4), timed with core 1's own cycle counter (DWT CYCCNT, enabled in `setup1`). A long pass is a register read left waiting. Console `r` clears it.

## [0.23.2] - 2026-10-30

### Added
- **`bus_timing.pio`, a read timing probe** (22 instructions, only reads pins, in the third PIO block when DMA isn't offered). For each read it measures t1 (chip select to DIOR# falling) and the strobe (DIOR# low), to ±20 ns. Core 0 prints the first 64 after start-up or console `r` as `RDTIME n t1_ns= strobe_ns=` lines; `s` shows `rd_timed`, `rd_t1_ns` and `rd_strobe_ns` (minimum-maximum). It sizes the no-IORDY design to a real host: on the Pentium's PIIX, IORDY was often not yet low when DIOR# fell (`iordy_early`), so t1 there is short.
- `pio_sim`: `in` shifting left (`in_shift_left`). `test_bus_timing_sim.py` checks t1 from 0 to 200 ns and strobes from 100 to 400 ns.

## [0.23.1] - 2026-10-30

On the Pentium, 0.23.0 got the BIOS as far as IDENTIFY, but data reads took 1.3-2 µs and 28 words were wrong: core 1 answered each request only after a pass of its main loop.

### Changed
- **`early_answers` gets a fast lane** (`ide_bus::serveDataInEarly`): during a data block, core 1 answers data requests from the sector buffer in a tight loop. A data read's report moves it on by one; anything else catches the device up and is served normally.
- **After any access, core 1 watches only the bus for `EARLY_BUS_ONLY_US` (50 µs),** skipping its bookkeeping, so the next request finds it free.
- `Device::dataInRemaining()` is always inlined (`check_core1_ram.py` found an out-of-line copy in flash).

## [0.23.0] - 2026-10-30

Early answers, for hosts that don't wait for IORDY (docs/no-iordy.md). A Pentium 200's PIIX runs in compatible timing, which ignores IORDY, until its Phoenix BIOS has identified the drive, and the BIOS never got that far. Its register reads ended before core 1 answered: the DEVICE read-back that checks a drive is there got `9004`, and 31 of IDENTIFY's 256 words were wrong. Experimental, and off by default.

### Added
- **`early_answers` in `HDDSYNTH.INI`:** runs `ide_bus_early` (in `ide_bus.pio`) in place of `ide_bus`. At the chip select it asks core 1 for the register's value, an answer request with bits 21-31 zero, so the value is in the TX FIFO when DIOR# falls, t1 (70 ns or more) sooner than now.
  - A write throws its answer away. So does an address or chip select that changes while armed; Y holds the address asked about, and the program asks again.
  - A read still reports itself, in a sample taken during the strobe, so core 1 applies its side effects.
  - After each access the program arms again, so the next data word is asked for before the host's next strobe.
  - IORDY is low from the chip select as before, released only when nothing is selected, so hosts that wait still can.
  - 32 instructions, so no `iordy_watchdog` (not with `isa`), and core 1's fast data path is off in this mode.
- Core 1 (`ide_bus::serve`): an answer request gets `peek()`, with no side effects. A read's sample gets `consumeRead()`. Writes are unchanged.
- Sim: `ide_host_model` can be a host that never waits for IORDY (`waits_for_iordy=False`), and keeps DA's t1 when the address changes with CS held. `test_pio_bus_early_sim.py`:
  - a never-waiting host gets register reads, IDENTIFY's 256 words and a written register's read-back right, as long as core 1 answers within 160 ns of the chip select;
  - hosts that wait still get their data in PIO modes 0-3;
  - an address change with CS held; flow-controlled writes; a chip select with no strobe.
- **Not yet run on any host.**

## [0.22.0] - 2026-10-30

`isa = on` done with a watchdog. 0.21.2's 60 ns releases didn't free the ICL 386: it froze again after F1. A released IORDY rises through the host's pull-up and the cable's capacitance, which takes far longer than 60 ns. And the old trace showed the BIOS waits up to 11 µs between a status read and its first data read, with the drive still selected, more than the bus program's countdown (31 passes at most) can hold.

### Changed
- **`ide_bus.pio` is one program for every host again,** with no countdown. IORDY goes low at the chip select (`arm`) and stays low until a strobe or the chip select ends, as up to 0.20.0. Down from 32 instructions to 23:
  - **Writes are sampled as DIOW# rises.** `wait 1 gpio 27` and then `in pins, 23` take DD, DA and the chip selects about 5 ns after the edge (ATA holds the data 10-30 ns, t4). `in null, 9` forces DIOW# and the bits above it to 0, so core 1 still reads the sample as a write. Before, the program kept the last sample taken while DIOW# was low, in a loop.
  - **Flow control moved to the push:** the autopush stalls with IORDY low while the RX FIFO is full, so the host's next access waits.
  - **A read's sample** comes from `in pins, 32` with autopush, and DIOR# is tested with `jmp pin`. The state machine starts at `idle`.

### Added
- **`iordy_watchdog.pio`** (7 instructions, in `ide_bus`'s PIO block, started with `isa = on`): when IORDY has been held low for 4,608 cycles (23 µs) without being released, it writes the pin's direction to released. `ide_bus` pulls it low again at its next `arm` or `read`, so a stalled cycle gets as long as the pull-up needs. Real accesses hold IORDY about 1 µs at most. The block always holds the watchdog, so the diagnostics' `iordy_check` can't land beside `ide_bus`'s DIOR# synchroniser bypass.
- Sim: `pio_sim` counts the bits shifted into ISR (partial `in` with autopush) and does `set pindirs`. `test_pio_bus_sim.py` passes in PIO modes 0-3 on the new program (writes included), and on the ISA host. `test_iordy_watchdog_sim.py` covers: a stuck hold released at 23.2 µs; reads, and a 15 µs hold, left alone; and holds that don't add up.
- **Not yet run on the USB adapter:** the write path changed for every host. Run slices 1-4 (writes) and `-m speed` there before trusting it.

### Removed
- 0.21.x's countdown and `isaProgram()` patching.

## [0.21.2] - 2026-10-30

On 0.21.1 the ICL 386 got through detection and the BIOS ran on, with no freeze. But its MBR read lost one word (`NOIORDY reads=1`), and the boot code stopped with part of a message ("ion"). The card keeps the drive selected while an address stays on the bus. After the status read, the BIOS took longer than the 2.6 µs timeout to start reading data, and the timeout had released IORDY until the chip select went.

### Changed
- **The ISA timeout repeats:** after 2.6 µs without a strobe, `timeout` releases IORDY for 60 ns (`jmp idle side 0 [7]`, then `idle` and `arm`) and pulls it low again, for as long as the chip select lasts. A stalled memory or refresh cycle ends at the first IOCHRDY sample (every 125 ns) that falls in a release, and a read is held whenever it comes. The ATA program never gets there.
- Tried and dropped: an 11.5 µs timeout made by delaying all nine steps of the armed loop. The loop then acted on a chip select sampled 120 ns earlier. The 386 deselects the drive briefly between a status read and the first data read, and the sim caught the data read losing IORDY (`test_isa_program_meets_ata_timing[0]`).
- Sim: the timeout's periodic release (held 2-3.2 µs, released 50-80 ns), and a read whose strobe lands anywhere in that cycle after a long chip select, which must have IORDY low within 80 ns.

## [0.21.1] - 2026-10-30

`isa = on` reworked: 0.21.0's IORDY from the strobe was too late for the ICL 386's 16-bit ISA cycles. IORDY went low about 76 ns after DIOR#; 12 data reads ended first (`iordy_ignored=12`), IDENTIFY reached the BIOS with 28 wrong words (`BUSERR`), and it then verified a nonexistent sector 64 times (IDNF). IORDY from the chip select (0.20.0) had read IDENTIFY cleanly.

### Changed
- **`ide_bus.pio` rearranged, with a countdown on a chip select that brings no strobe.** `arm` pulls IORDY low at the chip select, as ever, and loads Y. As assembled, Y = 2^32 - 1, which never times out (ATA hosts). With `isa = on`, `ide_bus.cpp` patches it to `set y, 31` and gives the `timeout` step 7 cycles' delay: about 2.6 µs without a strobe, then `timeout` releases IORDY until the chip select goes, ending a stalled memory or refresh cycle. That's longer than an ISA host's gaps between data reads, about 1.5 µs. A read after a timeout pulls IORDY low as it starts (late, as in 0.21.0).
- To fit the countdown's three instructions into 32: DIOR# is tested with `jmp pin` (from the pin, which skips the synchroniser) instead of from the sample; a read's sample is `in pins, 32` with autopush instead of `mov isr` and `push`; the write path falls into `arm` and the read path into `idle`. The state machine starts at `idle` (`pio_sm_init`, `restart()`).
- Sim: the PIO simulator gained autopush and `set`. All of `test_pio_bus_sim.py`'s PIO mode 0-3 timing tests pass on the rearranged program, and the ISA program passes them too. New tests: the ISA timeout (released 2.6 µs after a chip select with no strobe, and stays released), the ATA program holding IORDY throughout, and IDENTIFY bursts and flow-controlled writes on the ISA host.
- **Not yet run on the USB adapter:** the bus program every host uses has changed shape. Run slices 1-2 and `-m speed` there before trusting it.

## [0.21.0] - 2026-10-30

`isa = on`, for ISA IDE cards that pass IORDY to the ISA bus. On 0.20.0 the ICL 386 detected the drive, then froze for good while printing "Hard disk 0 auto initialization", with the keyboard dead. The pins showed CS0# low with DA2-0 = 0, IORDY low and no strobe, for minutes. Console `V` (the drive off the bus) released IORDY: it read high, so the card does wire it, and the address lines moved again. The card decodes CS0# from the address alone, so a memory cycle (or a DRAM refresh) whose address aliased 1F0h selected the drive. The bus program pulls IORDY low at a chip select, to meet ATA's 35 ns, and holds it until a strobe that a memory cycle never brings, while the card passes IORDY to IOCHRDY, which stalled that very cycle.

### Added
- **`isa` in `HDDSYNTH.INI`** (off by default; `CONFIG ... isa=`). On, `ide_bus::begin()` loads the bus program with three side-sets moved (`isaProgram()`, from the public labels `armed`, `read` and `write`): IORDY stays released in `armed` and goes low at `read` and while `write` waits for room. So a chip select with no strobe never touches IORDY. IORDY goes low at most 76 ns after DIOR# falls, against an ISA bus's first IOCHRDY sample about 125 ns in (`IsaBus` in `ide_host_model.py`). Too slow for ATA hosts, hence the setting. A write with room in the FIFO pulls IORDY low for one PIO cycle (5 ns): at most one extra ISA wait state.
- `test_pio_bus_sim.py`: the patched program against the ISA host (reads, IDENTIFY's 256 words, flow control on writes, and a chip select held for 10 µs with no strobe, which must leave IORDY alone), and checks that the default program still pulls IORDY low at the chip select. `pio_sim.assemble()` reads a program's public labels.

## [0.20.0] - 2026-10-30

IOCS16#, for 16-bit ISA IDE cards that pass it to the ISA bus. On 0.19.1 the ICL 386's BIOS got through detection, but every 16-bit data read reached the drive as two 8-bit reads, of the data register (1F0h) and then the error register (1F1h): 513 pairs for one sector and IDENTIFY. So each word's high byte came from the error register, and the BIOS took 15 heads and 17 sectors from the garbled IDENTIFY. Needs one wire on the rig: IDE pin 32 to GPIO 2 (EXT1-6).

### Added
- **`iocs16.pio`** (6 instructions): pulls IOCS16# (GPIO 2, open drain) low while CS0# is low and DA2-0 = 0, and only then, since an 8-bit register addressed as 16-bit would come back on the wrong byte lane. Two loops that set the pin on every pass, so it doesn't glitch. In simulation it follows the address within 40 ns, both ways (`test_iocs16_sim.py`; ATA-1 allows 90 ns).
- It needs a PIO block, and the diagnostics' has 3 instructions spare, so it takes `ide_dma`'s: `setup1()` starts `ide_dma` when multiword DMA is offered and `iocs16` otherwise. ISA hosts never use DMA. Every `ide_dma` entry point already did nothing until `begin()`.

### Changed
- `docs/wiring.md`, `docs/wiring.html` and CLAUDE.md: IDE pin 32 is IOCS16# on GPIO 2. GPIO 2 used to be kept off the bus for the BB48's 2.2k pull-up (R17), which is harmless for an open-drain line the host also pulls up. PCB rev 0.4 already plans it there.

## [0.19.1] - 2026-10-30

Two fixes for real ISA IDE controllers, found on the first retro PC (an ICL 386): its BIOS sent EXECUTE DIAGNOSTIC, never got the interrupt and reported "Hard disk controller error".

### Fixed
- **INTRQ is driven low between interrupts** while the drive is selected with nIEN clear, and released otherwise (`Device::intrqDriven()`, `ide_bus::updateIntrq(dev)`). It used to be driven high only, and released otherwise, relying on the host's pull-down (the USB adapters have one). The ICL's ISA card has none: the undriven line floated high (`p` showed `INTRQ=1` throughout), so the BIOS's edge-triggered IRQ 14 never saw the diagnostic's interrupt.
- **Reads of the control block other than Alternate Status aren't driven.** An ISA card's CS1# covers 3F0-3F7, where the floppy controller answers 3F2-3F5 and bit 7 of 3F7; the trace showed the BIOS setting up the floppy there, and the drive answered those reads too, against the floppy controller. Core 1's answer now carries which DD lines to drive in its upper half (all, or none for `Reg::None`), and `ide_bus.pio` drives them with `out pindirs, 16` in place of `mov pindirs, ~null`, so the program keeps its 32 instructions. `test_pio_bus_sim.py` checks CTL 2-5 and 7 reads leave DD alone, in PIO modes 0-3.

## [0.19.0] - 2026-10-30

Activity link protocol version 2, for the HDD Synth's modelled drives: where the head goes, and authentic seek timing. From docs/hddsynth-head-position-brief.md; the synth's side is HDDSynth_Native v0.21.0. Host-tested; not yet run on the rig or with the synth.

### Added
- **`$HDE`, head targets** (`activity_link.*`): `$HDE,2,<pos>,<op>,<sectors>*hh` for each media command, `pos` being its first LBA as a fraction of the disk (0-65535), so the synth's head goes where the host's commands really go. Reads and writes are sent when core 0 gets their first sector, verifies and seeks from their trace events. At most one every 2 ms, the newest (`HeadPacer`); a due `$HDA` goes first (`nextSentence()`). `s` shows `head_sent`, `head_replaced`, `head_last`.
- **Authentic seek timing** (`seek_timing` in `HDDSYNTH.INI`: `off` by default, `synth`, or the synth's presets `stepper85`, `voice91`, `voice97`, `fdb05`). `seek_timing::Scheduler` (pure, host-tested) works out when the modelled drive would have each sector (seek, rotation, transfer, on one microsecond clock), and core 0 keeps each fetch's result until then, so the drive shows BSY as a real one does; IORDY is never held for it. Reads, writes (with write-behind too) and DMA alike. A command carrying straight on from the last one's next sector doesn't seek or wait a turn (a drive's read-ahead), so sequential transfers run at the media rate. Capped at 1 s a command. With timing on, `$HDE` goes out when the modelled seek starts. Console `H` switches it off and on until a restart; `S` shows the setting and model, `s` shows `seek_timing`, `seek_active`, `seek_rpm`, `seek_actuator` and the counts (`seek_commands`, `seek_held`, `seek_avg_us`, `seek_max_us`, `seek_total_us`, `seek_capped`).
- **`$HDM`, the synth's model**, on the link's other wire: GPIO 1 is now UART0 RX, with the internal pull-up. Core 0 assembles lines on each loop pass; `seek_timing = synth` follows the model it names, and forgets it after 5 s without one. `s` shows `hdm_valid`, `hdm_rejected`, `link_rx_errors`.
- **`drive_params.*`, `drive_timing.*`, `sentence.*`**, copied unchanged from HDDSynth_Native (6a68a71, v0.21.0), with their host tests (less the one against the synth's engine).
- **`-m seek`** (`host/tests/test_seek_timing.py`): with `seek_timing` set to a preset, random 4 KB reads run within 20% of the modelled time, sequential reads at about the media rate, and with `H` off as before.

### Changed
- **`ata::Fetch`** carries the command's first LBA and sector count (core 1 sets them from the command it already decoded), so core 0 can tell where each command starts. It grows from 16 to 20 bytes.
- **The loopback test (`U`)** checks both directions: `$HDA` and `$HDE` read back, plus one `$HDM` it sends itself and reads back. `LOOPBACK` adds `head_sent`, `head_valid` and `model=ok|missing|bad`; GPIO 1 no longer changes function for it.
- `HDDSYNTH.INI`'s defaults (and the welcome disk's copy) list `seek_timing = off`; the README lists it.

## [0.18.1] - 2026-10-30

The bus observer no longer holds multiword DMA back: spot checks by default, full checks on request.

### Changed
- **Spot checks** (`bus_observer.*`, the default). Checking a sector costs core 0 more than serving it, so at DMA's rate the observer slowed the drive to about 2.2 MB/s on the test disk. Now the drive comes first: core 0 serves fetches every 32 words checked, and when the checks fall more than half the ring (2,048 samples) behind, the waiting samples are dropped, counted in `bus_dropped`. Checking resumes at the next sector start, found from its header (the LBA, then the LBA inverted) even in the middle of a DMA command, or at the next status read. Every sector of PIO and of short DMA commands is still checked; on long DMA transfers a few percent. On the test disk, MDMA 2: sequential reads 7.70 MB/s (5.14 on 0.16.2, before the observer checked DMA; 2.0 with full checks), random 4 KB reads 1,100 a second (969), writes 13.9 MB/s (9.76).
- **Full checks** (console `O`, until a restart): every sector, as before, slowing DMA reads to about 2 MB/s. `s` shows `bus_checks=spot|full`. The soak switches to full checks itself (`HDDPRO_SOAK_SPOT_CHECKS=1` keeps spot checks).

### Fixed
- **Overruns the observer couldn't see.** Its samples are numbered in 11 bits (2,048), and since 0.17.1 its ring holds 4,096, so core 0 falling a whole ring behind left no jump in the numbers. Spot checks now treat any spell of more than 400 µs between looks at the ring as a possible overrun (DMA could fill it in 490 µs), drop what's waiting, and count it in `bus_lost`. Full checks slow the drive instead, so the ring isn't overrun (the 10-hour soak's high-water mark was 2,570); the soak's 0 bad sectors show it wasn't.
- After dropped samples or a gap the checker waits for a sector boundary, rather than taking the next 256 words for a sector: in a DMA command those could be two sectors' halves.

### On the rig
- Test disk, MDMA 2 (`host/runs/tools/observer_load.py`, 48 MB in 64 KB reads): spot checks 8.77 MB/s, 767 sectors checked, 0 bad; full checks 2.00 MB/s, all 98,320 checked, 0 bad, 0 dropped. LBA 65535 5,000 times: 0 flagged. Slices 3-4 pass; a 3-minute DMA soak (full checks): 304 MB, 522,221 sectors checked, 0 bad. PIO mode 3 on the test disk: reads 1.38 MB/s, writes 1.87, as 0.18.0.

## [0.18.0] - 2026-10-29

A welcome disk with a README, instead of the test disk, when there's no card or no image to serve.

### Added
- **The welcome disk** (`src/welcome_disk.*`, host-tested): with no microSD card, or no image it can use, the drive serves a 64 MB disk with an MBR partition table and one FAT16 volume, `HDDSYNTHPRO` (partition type 06h, 2 KB clusters, ending with the last whole cylinder of IDENTIFY's 16-head, 63-sector geometry). It holds `README.TXT`, how to set the drive up, and `HDDSYNTH.INI` with the defaults, ready to copy to a card. Every sector is computed when read, like the test disk's, so it costs no RAM; writes go to the same RAM overlay, until a restart. Neither the MBR nor the boot record boots: both run INT 18h.
- **`welcome/README.TXT`**, the README's text, with ASCII art. `scripts/embed_welcome.py` checks it (plain ASCII, at most 80 characters a line), turns its line endings into CRLF, fills in `{VERSION}` and writes `src/welcome_text.h` (git-ignored) before each build (`extra_scripts` in `platformio.ini`) and before the host tests.
- **`.pio/native-tests/make_welcome_img`**, built by `test/run_native_tests.sh`: writes the welcome disk to a file, to see the README as a computer will before flashing. macOS's `fsck_msdos` passes it, and mounts it as `HDDSYNTHPRO` with both files.
- **Console `T`**: restarts the drive serving the test disk, card or no card, or back. It shares `J`'s watchdog scratch register, so a power cycle undoes it, and `T` and `J` undo each other. `s` shows `disk=welcome` or `disk=generated`.

### Changed
- **The test disk is served only on request** (`image = none`, or `T`), no longer whenever there's no card. The bus observer checks data sectors only on it, as before. Rig tests that need it (slices 3-4, the soak, `-m timing`) now skip on the welcome disk too, and say how to get the test disk.
- `RamOverlayDisk` takes the disk underneath at boot (`setBase()`), so the welcome disk and the test disk share its 256 KB.
- The defaults in `HDDSYNTH.INI` (console `C`, and the welcome disk's copy): the image falls back to the welcome disk.

### Fixed
- The host tests didn't compile on macOS: `activity_link::LINE_MAX` clashed with the `LINE_MAX` macro in macOS's `<limits.h>` (not seen on Windows or Linux). Now `MAX_LINE`.

### On the rig
- **Windows** (30 Oct, no card): mounts the welcome disk as `HDDSYNTHPRO` (FAT, healthy) with `README.TXT` and `HDDSYNTH.INI`, and adds its `System Volume Information` folder, which fits in the overlay. Slices 1-2 pass on it (20 re-detections each). `-m speed` on the test disk (`T`), PIO mode 3: reads 1.41 MB/s, writes 1.87, worst read 1,030 ns (0.17.x: 1.36 and 1.79), so the core 1 changes cost nothing.
- **Windows keeps the old disk's partitions after a switch**: after `T` from the welcome disk it still listed D: on the test disk, and refused raw writes inside it (`EBADF`), failing `-m speed`. The rig's Windows host now runs `Update-Disk` before opening the disk for writing, which makes Windows read the partition table again (`host_windows.py`; not firmware).
- Still to check: macOS mounting it (`.fseventsd`, `.Spotlight-V100`, in the 256 KB overlay).

## [0.17.1] - 2026-10-29

The three bus observer findings from the 6-hour DMA soak, fixed.

### Fixed
- **LBA 65535's word 3 in multiword DMA** (`ide_dma.pio`). That word takes all 16 data lines from 0 to 1 at once, and the switching was enough for the DMA read program, which watches DIOR# without the input synchroniser, to see the strobe end early and release DD before it had. The host had already latched the word (it read it right all through the soak), but the bus observer, sampling as DIOR# rises, saw `0000` in about one read of that sector in four. The program now waits 4-5 cycles (20-25 ns) after driving DD before watching DIOR#, well inside mode 2's 70 ns strobe (tD). On the rig (`host/runs/tools/dma_settle.py`, 5,000 reads of LBA 65535): 24% flagged before, 0 in 3 runs (about 7,600 checked) after; the host 0 wrong throughout. Stronger drive made it worse, as more switching noise would: 8 mA with fast slew failed the read at the host every time, so DD stays on the RP2350's default 4 mA, slow slew.
- **The observer's ring is 16 KB** (4,096 samples, from 1,024), and `s` shows its high-water mark, `bus_ring_max=used/size`. Sequential DMA reads on the built-in disk now peak at about 1,030 samples waiting, just over the old size: hence the soak's 94,071 missed words. 48 MB read in 64 KB requests: 0 gaps.
- **`bus_dma_words` is 64-bit**: at DMA's rate 32 bits wrap in a few hours.

### On the rig
- **A 60-minute DMA soak** (29 Oct, `soak_dma.sh 60`, built-in disk, write-behind on): passed. 6,455 MB, no mismatches, 221,957 commands all DMA, no resets, worst read 1,150 ns. The bus observer checked 11.1 million sectors: 0 bad (the 6-hour soak's rate would have given about 22), 0 gaps, `bus_ring_max` 1,034 of 4,096, and `bus_dma_words` 3.03 billion. The soak's progress lines now show `ring max`.
- **A 10-hour DMA soak** (29-30 Oct, `soak_dma.sh 600`, the same setup, with the soak's hourly 11-minute idle pauses): passed. 51.9 GB moved (693,681 reads, 375,843 writes), no mismatches, 1,791,444 commands all DMA, no resets, worst read 1,170 ns. The bus observer checked 89.0 million sectors: 0 bad and 0 gaps, through eight idle pauses (where 0.17.0's soak lost most of its words); `bus_ring_max` 2,570 of 4,096 (it rose in steps, to 2,322 by 4 hours and 2,570 at 8); `bus_dma_words` 24.4 billion. 5.2 GB an hour with the pauses, as 0.17.0's 6-hour soak.

## [0.17.0] - 2026-10-29

Write-behind on by default, and settings for it and the other options in `HDDSYNTH.INI`.

### Added
- **Four on/off settings in `HDDSYNTH.INI`** ([docs/settings.md](docs/settings.md)): `dma` (whether `mode = auto` may offer multiword DMA where DMARQ is wired; off), `write_behind` (on), `read_ahead` (on) and `activity_link` (on). They take on/off, yes/no, true/false or 1/0; a bad value or a repeat is a warning, as for the other settings. Console `E` sets them, and adds one the file doesn't have yet. `S` shows them all. The console's `W`, `A` and `L` still switch them until the next restart.
- The file `C` creates has them, with comments, and no longer says the firmware supports PIO mode 0 only.

### Changed
- **Write-behind is on by default.** Re-measured on 0.16.5: card writes 1.65 MB/s against 1.05, 192 MB written and read back clean, worst read 1,040 ns (its slow reads at 150 MHz are gone). The drive acknowledges a sector while storing it, so a power cut in that millisecond loses it; this adapter never sends FLUSH CACHE. `write_behind = off` puts it back.
- `cfg::AUTO_DMA` is now the `dma` setting (default `drive_config::DEFAULT_DMA`, off until the DMA soak).

### On the rig
- **A 20-minute DMA soak** (`host/runs/tools/soak_dma.sh 20`, the dry run for the long one): 2,242 MB, 0 mismatches, 77,415 commands all DMA, 3.84 million sectors checked on the cable, 0 bad, worst read 1,150 ns, no host resets. About 112 MB a minute, twice the PIO mode 3 soak's rate, with the observer checking every DMA word.
- **The 6-hour DMA soak** (29 Oct, multiword DMA mode 2, built-in disk, write-behind on): passed. 32.9 GB moved in 6 hours (439,693 reads, 237,479 writes), no mismatches, 1,134,579 commands all DMA, no host resets, worst read 1,150 ns; about 92% more data than the PIO mode 3 soak in the same time. The bus observer checked 51.6 million sectors and flagged 102, every one LBA 65535 word 3 (`0000` seen where `ffff` was due, all 16 lines rising at once, which the host read correctly): the bus still settling when the observer samples, up to about 15 ns before the host does, so later than DMA mode 2's 50 ns (tE). The data lines run on the RP2350's default 4 mA, slow-slew drive, the first suspect. It also missed 94,071 words (`bus_gaps`), almost all after the first idle pause: core 0 falling behind its 1,024-word buffer at DMA's rate. And `bus_dma_words` wraps at 2^32. All three to fix before DMA becomes the default.
- **DMA writes to the card**, with write-behind (the scratch image, `writeverify.py`): 2.23 MB/s against 1.65 in PIO mode 3, 128 MB written and read back clean, worst read 1,150 ns. The card is the limit (the built-in disk takes DMA writes at 9.8 MB/s).

## [0.16.6] - 2026-10-29

Ready for the PCB: the firmware switches its IDE bus on.

### Added
- **BUS_EN# (GPIO 41)**: rev 0.3's bus switches stay off until the firmware drives it low. Core 1 does, right after `ide_bus` has set the bus pins up, so the host never sees them in another state; the listen build does too, after setting its pins up. On the BB48 GPIO 41 is unconnected. With it, JP1 (which bypasses BUS_EN#) can stay open.
- `p` also shows `SD_CD=` (GPIO 40, the PCB's card detect; it floats on the BB48) and `BUS_EN#=`.
- [docs/pcb-bringup.md](docs/pcb-bringup.md): the first day with the board, step by step.

### Changed
- **The DMARQ test runs on core 1, after the bus switches are on** (it ran on core 0 before the bus was set up, so on the PCB it would have found the pin bare and never offered DMA). Core 1 also decides the DMA offer there, before anything builds IDENTIFY. On the rig: still `dmarq=wired`, reads unchanged.

## [0.16.5] - 2026-10-29

Catches a host that doesn't wait for IORDY, before trying a retro machine.

### Added
- **`iordy_check.pio`**: counts reads the host ended (DIOR# rising) while IORDY was still low, so it didn't wait for the data. ATA lets hosts ignore IORDY in PIO modes 0-2, and some older controllers do; they'd read garbage whenever core 1 is slower than their strobe. `s` shows `iordy_ignored=` and `iordy_ignored_reg=` (should be 0), and a `NOIORDY` line appears, at most once a second, if it isn't. It runs in the diagnostics' PIO block beside the IORDY timer and the bus observer (now 29 of its 32 instructions). Simulator tests: a host that waits, one that doesn't, back-to-back data reads where `ide_bus` pulls IORDY low again 20 ns after each strobe, and DMA strobes.
- On the rig, through the USB adapter (which honours IORDY): 0 across detection, reads and `-m speed`.

## [0.16.4] - 2026-10-29

The drive finds out at start-up whether DMARQ is wired.

### Added
- **DMARQ detection:** before choosing the disk and modes, core 0 turns on GPIO 31's internal pull-up (only pull-downs are forbidden) and reads it 1 ms later. The host pulls DMARQ down, so a wired pin reads 0 and a bare one 1; the pull-up goes off again. `S` shows `CONFIG dmarq=wired|absent auto_dma=0|1`. On the rig: `wired`.
- **DMA is never offered without DMARQ**, even when `HDDSYNTH.INI` asks for it (`mode = mdma2`): `S` warns, and the drive offers PIO. A host told the drive does DMA would otherwise wait for DMARQ forever.
- **`cfg::AUTO_DMA`**: whether `mode = auto` offers DMA when DMARQ is wired. Off until DMA has had a soak; then `auto` gives DMA on the PCB and the wired rig, and PIO mode 3 on a BB48 without the wire. On the PCB the bus switches must be on (BUS_EN#) for the test to see the host.
- Not yet tried on the rig: a BB48 without the wire reading `absent`.

## [0.16.3] - 2026-10-29

The bus observer checks DMA data on the cable.

### Added
- **The bus observer checks multiword DMA reads**, as it does PIO data reads. It already recorded every DIOR# strobe; the checker (`sector_check`) now takes a read with no chip select, which is a DMA word, as data. A DMA transfer is whole sectors back to back, and the status poll after each command starts a new sector, so they stay in step. `s` shows `bus_dma_words=`. Three new unit tests, one replaying 0.16.1's lost first word (caught).

### On the rig (built-in disk, `mode = mdma2`)
- Slices 3-4 and `-m speed` pass with the observer checking: 331,090 sectors checked on the cable, 0 bad, 0 gaps, from 89 million DMA words (the 17,188 skipped were the tests' own random writes read back). Worst read 1,030-1,100 ns.
- **With the observer checking every DMA word, reads on the built-in disk run at 2.60 MB/s instead of 5.14**: core 0 now computes each sector twice, once to serve it and once to check it. Writes (9.77 MB/s) and images (where the observer checks only IDENTIFY) aren't affected. A soak checks everything; if full speed is wanted under test, the checker could skip sectors when it falls behind.

## [0.16.2] - 2026-10-29

Multiword DMA works on the rig: reads 3.8 times faster, writes 5.5 times, random reads 12 times.

### Fixed
- **The first word of every DMA read burst was lost.** `ide_dma_read` raised DMARQ on its `pull` of the first word, and side-set takes effect when an instruction issues, even one that then waits. Core 1 puts the burst's count in the FIFO and only then starts the DMA channel, so DMARQ rose while the FIFO was empty, and the host's first strobe found nothing on DD: word 0 of every sector read 0048h on the rig, the other 255 right. DMARQ now rises on the first instruction after the pull. The simulator's DMA model now brings the words a little after the count, as the hardware does (`dma_delay_ns`); with the old program 9 of its 15 DMA tests fail.
- **A status poll after each DMA command was held 1,540 ns.** `ide_dma::endCommand()` reset the whole program even after a command that finished normally, in one bookkeeping step, just as the host polls. It now only switches DMARQ off then; the full reset is kept for a command cut short mid-burst.
- The trace names READ DMA's no-retry form and WRITE DMA (`ca ?` before). The activity classification and the HDD Synth link already counted DMA commands as reads and writes.
- Rig tests count DMA commands as well as PIO ones (slices 3-4, `-m speed`).

### On the rig (Windows, the built-in disk, with DMARQ and DMACK# wired; `mode = mdma2`)
- **The adapter takes multiword DMA mode 2** (SET FEATURES 03h, 22h) and reads and writes by READ DMA and WRITE DMA.
- Against PIO mode 3 on the same disk (`-m speed`): sequential reads 5.14 against 1.36 MB/s; random 4 KB reads 969 against 78 a second; sequential writes 9.76 against 1.79 MB/s. **The adapter's 10.9 ms per read command is gone (0.11 ms)**: in DMA the drive tells the adapter it's ready, where in PIO the adapter waited about 10 ms before polling. Reads now cost 98 µs a sector on the drive's side (the bus, fetches from the built-in disk, and a burst per sector), where the bus itself would allow 31.
- Data: slices 3 and 4 pass in DMA (the whole disk read and compared, writes read back), and 8,200 sectors read and checked by hand, 0 wrong. Six redetect cycles took DMA every time, no slow reads, no lost disk. Worst read 1,080-1,100 ns. The bus observer can't check DMA data yet.

## [0.16.1] - 2026-10-29

### Fixed
- **DMARQ (GPIO 31) was left with the RP2350's internal pull-down on**, its power-on default: `ide_dma` never turned it off. The rules forbid internal pull-downs (erratum E9), and the host pulls DMARQ low anyway. Off now.
- DMACK# (GPIO 3) is set up as an input with no internal pulls, so it can be read.

### Added
- `p` also prints `PINS DMARQ=... DMACK#=...` (drive build). With the two DMA wires added on the rig (29 Oct): DMARQ 0, DMACK# 1 at rest, as expected; idle levels can't prove the wires, DMA working does.

## [0.16.0] - 2026-10-28

Multiword DMA, built but not yet tried on hardware: it needs DMARQ wired.

### Added
- **READ DMA and WRITE DMA** (C8h-CBh) in multiword DMA modes 0-2 ([docs/dma.md](docs/dma.md)), offered only when `HDDSYNTH.INI` says `mode = mdma0`, `mdma1` or `mdma2`: on the BB48 rig DMARQ and DMACK# aren't wired yet, and a host told the drive does DMA would wait for DMARQ forever. The PIO mode offered alongside is `auto`'s (3).
  - `ide_dma.pio`: the data phase, in a PIO block of its own, fed (reads) or drained (writes) by an RP2350 DMA channel, since DMA has no IORDY and mode 2 gives 50 ns from DIOR# to data. Proven in the simulator against a multiword DMA host in modes 0-2 (`host/tests/test_dma_sim.py`).
  - `ide_dma.cpp`: core 1 runs a burst per sector, handing DD0-15 to the DMA block for a read burst and back to `ide_bus`'s after it, and drives DMARQ (GPIO 31) only during a DMA command (its output forced off otherwise). A command cut short (RESET#, SRST, a new command, an error) stops the burst and releases both.
  - `ata_device`: the same fetches, read-ahead and write-behind as PIO; one interrupt, at the end; no BSY between sectors. IDENTIFY word 49 bit 8 and word 63 (modes supported, the fastest selected), SET FEATURES 20h-22h. 10 new unit tests.
- `cfg::PIN_DMARQ` (GPIO 31) and `cfg::PIN_DMACK_N` (GPIO 3), as on the PCB. `S` shows `mdma_offered=`.

### On the rig
- With DMA not offered (the default), nothing changes: slices 3 and 4 pass in PIO mode 3, reads 1.38 MB/s and writes 1.79, and the bus observer checked 335,019 sectors on the cable, 0 bad (1 gap: words it missed, not wrong ones). The worst read during the tests was 1,030 ns (920 on 0.15.0): the new code moved core 1's in RAM.
- **10-hour soak** (Windows, generated disk, PIO mode 3, 200 MHz; 28-29 Oct): clean. 28.9 GB moved (385,288 reads, 209,009 writes), 0 mismatches, 995,797 commands, 49.5 million sectors checked on the cable with 0 bad and 0 gaps, no host resets, worst read 1,060 ns (steady all night). Against the 0.12.0 soak (150 MHz, PIO mode 0) at the same point: 7.8% more data, and 180 ns more margin on the worst read.

## [0.15.0] - 2026-10-28

PIO modes 1-3: the adapter now runs the bus in mode 3.

### Added
- **PIO modes 1, 2 and 3.** IDENTIFY offers modes up to `HDDSYNTH.INI`'s `mode` (word 51 for 1-2, word 64 for 3, words 67-68 the cycle time), and SET FEATURES accepts flow-control modes up to the one offered. `auto` offers mode 3, the fastest there is (`drive_config::AUTO_PIO_MODE`). The bus program is the same in every mode: IORDY holds the host while core 1 answers, as in mode 0.
- `S` shows `pio_offered=` in the CONFIG line.

### On the rig (Windows)
- **The adapter takes mode 3**: it asks for mode 4 (SET FEATURES 0Ch) and, refused, for the fastest IDENTIFY offers in word 64 (0Bh). It never asks for modes 1 or 2: offered mode 2, it fell back to mode 0.
- On the generated disk, mode 0 against mode 3: reads 1.33 against 1.36 MB/s (297 against 287 µs per sector), writes 1.58 against 1.79 MB/s (+13%). Reads barely change because the adapter, not the bus, sets their pace: about 1.1 µs a data word in both modes, where the simulator gives the bus program about 440 ns a word in mode 3. So faster PIO modes can't speed reads up much on this adapter; DMA modes may (on the PCB, which has DMARQ and DMACK#).
- Mode 3's data checked: slices 3 and 4 pass (reads against the generated disk, writes read back), and the bus observer checked 262,868 sectors on the cable, 0 bad. `iordy_early` now counts most reads: in mode 3 the chip selects come only 30 ns before the strobe, so IORDY falls just after it (about 10 ns), still well before the host samples it (35 ns).

### Simulator
- The host model has modes 1-4 (`ide_host_model.PIO_MODES`, `FAST_PIO_MODES`), with mode 3-4's strobe recovery (t2i), and records IORDY falling after the strobe as a negative lead. `test_pio_bus_sim.py` holds the bus program to modes 0-3's timings (IORDY low at least 15 ns before the host samples it: 24 ns in mode 3).
- **Mode 4 is not offered**: its write data is valid only 20 ns before DIOW# rises, and the program samples a write every 35 ns, so the last sample can miss it (the simulator loses words); and a read straight after a write leaves IORDY 6 ns. It needs a reworked write path.
- Fixed: `test_iordy_timer_sim.py` and the slice 3 test's read past the end of the disk (Windows refuses it, macOS returns nothing).

## [0.14.0] - 2026-10-28

200 MHz instead of 150: headroom on every read.

### Changed
- **The RP2350 runs at 200 MHz** (`board_build.f_cpu` in `platformio.ini`; Raspberry Pi certifies it), up from the default 150. Core 1 answers every read in instructions, so the time a read is held shrinks with the clock: the worst read during traffic and detection fell from 1,173-1,320 ns to 850-920 ns.
- **The peripheral clock stays on the system clock** (`peripheralClockFromSystem()`, before SPI and the link's UART start). Setting another clock moves it to the 48 MHz USB clock, where the card's SPI would top out at 24 MHz; at 200 MHz, 25 MHz is ÷8, exact as ÷6 was at 150.
- The bus program's data settle before IORDY is released is 8 cycles: 40 ns now, 53 before. ATA asks only that the data be valid when IORDY rises (tRD = 0), and the delay was already at its maximum. The PIO simulator runs at 200 MHz (`pio_sim.CLOCK_HZ`), and `ide_bus.pio` meets every ATA mode 0 timing there; the IORDY timer's test works its threshold out from the clock, as the firmware does.
- `s` shows `clk_sys_mhz=` and `clk_peri_mhz=`.

### On the rig (Windows)
- Reads 1.31 MB/s, up from 1.23 (`-m speed`: 10.9 ms per command + 301 µs per sector, against 318-328), worst read 850 ns.
- Detection: 12 redetect cycles, 0 slow reads (0.13.0: 2 in 24, 1,253-1,320 ns), worst 850 ns; 29 IDENTIFY blocks checked on the cable, 0 bad.
- Writes to `HDDBENCH.IMG`: 0.94 MB/s with write-behind off, as before. With write-behind on (`W`), 1.61 MB/s, and 160 MB written and read back with 0 bad sectors and 0 slow reads, against about 2,000 reads held 1.5-2.0 µs per 16 MB at 150 MHz; but the worst read was 1,250 ns, the limit itself, so write-behind stays off by default for now.

## [0.13.0] - 2026-10-28

A microSD card put back while the drive runs is served again, if it's the same card with the same image.

### Added
- **Re-mounting the card** (`src/media_watch.*`, host-tested; `sd_card.cpp`). A card pulled out is noticed when an image read or write fails: the drive prints `CARD lost`, and from then on the image's reads and writes fail at once instead of each waiting on the card. Every 250 ms core 0 looks for the card coming back, with a single CMD0 sent by hand (about half a millisecond; SdFat's own start-up retries it for 2 s with no card). A card that answers is started again and the image reopened, and it's served again (`CARD back HDDSYNTH.IMG`) only if it's the same card (its CID's maker and serial number) and the image is unchanged: the same size, place on the card and modify time (the drive never sets that time, so only a computer changes it).
- **Anything else isn't served**: `CARD changed: another card` (or `image modified`, `image replaced`, `image not found`, `can't mount`) and reads and writes keep failing until the right card is back, or the drive restarts. The host caches what it knows of the disk's file system, and its next writes would corrupt a different card or an edited image. While a different card is in, the drive checks once a second that it still is (reading its CID).
- **A card pulled and put back while the drive is idle costs nothing**: a read or write that fails is retried once after restarting the card, if it comes back as the same card.
- `s` shows `card=` (`none`, `present`, `lost` or `changed`), `card_lost=` and `card_back=`.
- **`-m card`** (`host/tests/test_card.py`): the card pulled and put back while idle and during reads, with the data compared before and after; with `HDDPRO_SECOND_CARD=1`, another card refused. Needs someone at the rig; reads only.

### On the rig
- Card in: reads 1.23 MB/s and writes to `HDDBENCH.IMG` 0.92 MB/s, as before, worst read 1,226 ns.
- `-m card` on Windows passes. A swap while idle is invisible: the next read found the card restarted, restarted it and was retried. Pulled during reads: the read in progress failed after 345 ms (UNC) with `CARD lost`, later ones in about 170 µs. The adapter resets and re-detects the drive about every 21 s and retries the read: with the card back within 20 s the retry succeeded and the host saw no error, only a slow read; left out 90 s, the host saw four failed reads, then the fifth retry succeeded. Either way Windows kept the disk and the data read the same afterwards. Not yet tried: a second card (`HDDPRO_SECOND_CARD=1`).
- Starting with no card, tried on the rig at last: the built-in disk after 2.0 s (`SD error: no card`, `card=none`), and Windows reads its pattern right.
- The overnight soak on 0.12.0 (generated disk, Windows): 10 hours clean. 26.8 GB moved (357,427 reads, 192,546 writes), 0 mismatches, 920,973 commands, 45.9 million sectors checked on the cable with 0 bad and 0 gaps, no host resets, worst read 1,240 ns (ALTSTATUS).

## [0.12.0] - 2026-10-27

Serve the built-in test disk with the card in.

### Added
- **`image = none`** in `HDDSYNTH.INI` (any case): serve the built-in 64 MB test disk instead of an image, leaving the card alone. The soak test needs it (only that disk's data can be checked on the cable by the bus observer), and until now it was served only with no card in. Set it with console `E image = none` and `!`; `E image = HDDSYNTH.IMG` and `!` put the image back. `J` still comes first. The file `C` creates mentions it.

## [0.11.1] - 2026-10-27

The STATUS read after IDENTIFY, within ATA's limit (nearly always).

### Fixed
- **The adapter's first status poll after a command was held 1,346-1,386 ns** (ATA allows 1,250): it polls at once after IDENTIFY and SET FEATURES 03h/08h, while core 1 was still on the command's trace bookkeeping or its next bookkeeping step. Core 1 now answers any access already waiting before that bookkeeping, and for 4 µs after a command starts (`cfg::COMMAND_QUIET_US`) only serves the bus. Recording the CMD line or posting a READ's fetch 4 µs later costs nothing; the adapter waits about 10 ms after a READ.

### On the rig (Windows, redetect cycles)
- Measured in A-B-A order against v0.11.0 after a PC restart, which cleared the adapter's decline (below): v0.11.0 6/24 double detections, 2 lost or renamed disks, 50 slow reads; this fix 2/24, 0, 4 slow reads (1,253-1,320 ns, around IDENTIFY: 4 in 48); v0.11.0 again 3/12, 2, 26. Reads 1.23 MB/s, worst read during traffic 1,173 ns, as before.
- Before the restart the same fix had looked far worse (11/12 double detections). The adapter was then declining in every build, the baseline included, after the PC had run for days with it plugged straight in: it was corrupting its own USB serial number and the disk's name. Restarting the PC cleared that, and 24 cycles on v0.11.0 afterwards didn't decline.

## [0.11.0] - 2026-10-27

Re-detection without a replug, and two detection bugs it found.

### Added
- **Console `V`**: the drive leaves the bus (the bus program stopped, DD, IORDY and INTRQ released, the device reset) and comes back at the adapter's next RESET# assert, a second `V`, or after 120 s. With a read sent meanwhile, the adapter fails it, resets about 20 s later and probes; the drive is back before that reset ends, so it is detected again (IDENTIFY, SET FEATURES) and the read is retried. About 30 s, and the host keeps the disk. Coming back later, after the adapter had started probing an empty bus, once left it with a garbled IDENTIFY.
- **`HDDPRO_REPLUG=redetect`**: `rig.replug()` does that cycle (`Host.redetect()`), so slices 1-2 run their 20 cycles with no hands on the rig. A re-detection, not a power-up probe: not for slice 0.
- **IDENTIFY checked on the cable**: the bus observer compares IDENTIFY blocks with the drive's own on the words that never change (serial, firmware, model, capacity), and prints `BUSERR t IDENTIFY ...` for a bad one; `s` shows `bus_identify=` and `bus_identify_bad=`. On an SD image only for 10 s after each RESET# release (detection), since checking every word costs core 0 enough to slow reads from 1.23 to 1.16 MB/s.
- **`STUCK`**: a command running over 3 s prints the last fetch core 0 served and whether its result is still waiting for core 1.

### Fixed
- **The drive stayed BSY for good when the host wrote a command over an unread block.** The JMicron sometimes gives up on a READ (or IDENTIFY) partway and writes the next command with no reset. `startCommand` kept the old data pointer, so the device looked mid-transfer: loop1 then never took the new fetch's result (`STUCK ... results_waiting=1`), and the new sector would have been taken for a read-ahead. Windows dropped the disk. A new command now abandons an unread block and any read-ahead; host tests reproduce both cases.
- **Busy (the activity LED and the HDD Synth link's busy flag) stayed on after a command was cut short** by RESET#, SRST or a new command, since it never counted as completed. After a detection the link said busy until the next command ended. A cut-short command now counts as completed.

### Found
- The STATUS read straight after each IDENTIFY is held 1,266-1,293 ns (now 1,346-1,386 with the fix above in the command path): the adapter polls at once while core 1 is still running the command. Deferring the command's trace bookkeeping only moved the slow read to SET FEATURES, so that was reverted. Slice 1's IORDY check fails on it.
- The JMicron read IDENTIFY with three words in four zero twice (once after a redetect that came back late, once after a reflash), and Windows named the disk "HD P". Not seen in 69 IDENTIFYs checked on the cable since.

### On the rig (Windows, no hands)
- 30 redetect cycles, each followed by 4 MB of reads from the SD image: all detected, 69 IDENTIFYs checked on the cable with none bad, 8 abandoned READs recovered (each a hang before the fix), the disk kept its name. Before the fix: hangs in about 1 cycle in 5. Reads 1.23 MB/s outside the IDENTIFY window.

## [0.10.2] - 2026-10-27

The slice 0 test, reworked so it can pass while the adapter probes forever.

### Added
- **The listen build's capture window** (console `w`, `cfg::LISTEN_WINDOW_EVENTS`): with it on, each RESET# assert opens a window of 3,000 bus accesses, and the rest are counted (`skipped=` in `s`) but not recorded until the next RESET#. Two windows fit in the 8,192-event ring, so the adapter's double reset pulse never drops an event, and the trace goes quiet between pulses. Off by default, so recording a real drive works as before.
- `trace.opening_sequence()`: the first steps of a trace in printed order, repeats merged and read data left out (the bus floats with no drive), for comparing one probe with another.

### Changed
- **`test_slice0`** turns the window on, asks for a replug, and judges the trace from the first RESET# release: no stalls, drops or overlapping strobes, STATUS read or DEVICE written, and the opening sequence matches the saved reference. The reference is kept per adapter, `host/reference/slice0_probe_<adapter>.txt`, named by `HDDPRO_ADAPTER` in `host/rig.env` (adapters probe differently).

### On the rig (Windows, no replug)
- The adapter on the Windows rig enumerates as a JMicron (USB `152D:2338`), not the NS1081. With the listen build on, a read from Windows failed after 23 s, then the adapter pulsed RESET# (first a double pulse: 49 ms low, 4.4 ms high, 49 ms low; then one every 4.78 s) and ran the device-presence check after each (DEVICE=A0, SECCOUNT/LBA=A5/5A/AA/55, read DEVICE and STATUS back), about 420,000 accesses a second. Over 90 s: 18 windows of 3,000 accesses, 0 dropped, 0 stalls, 33.5 million skipped; 16 of 18 openings identical (the other two: inside the double pulse, and the window cut off when the recording ended). The test's checks pass on that trace.
- Back on the drive build the adapter found the drive again by itself; Windows had dropped the disk, and `pnputil /restart-device` on the adapter brought it back without a replug.
- Still to do with a physical replug: `scripts/rig.sh -m slice0 --save-reference`, then a second run to compare.

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
