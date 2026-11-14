# PCB rev 0.4: what to add

**Ordered 10 Nov 2026**, from the files in `hardware/production/` (merged into `main` the same day). What follows is the plan and what went into it.

Rev 0.3 (the `hardware/` KiCad project, sent for manufacture on 28 Oct 2026) is the minimum that runs today's firmware: the IDE bus behind four SN74CB3T3245 bus switches, DMARQ and DMACK#, microSD wired for SDIO with card detect, the activity link, USB-C and the 5 V inputs. [pcb-bom.md](pcb-bom.md) holds the fuller design targets and their reasons.

This page is rev 0.4's plan. It was first written on 29 Oct, and revised on 8 Nov with what the retro PCs, the Amiga, the DMA soaks and the CD-ROM drive have taught since. GPIOs go into `src/config.h` first, and `hardware/tools/check_netlist.py` checks the board against it.

**Freeze the layout only after a day with rev 0.3** ([pcb-bringup.md](pcb-bringup.md)). Three of its results decide parts of rev 0.4:
- **SDIO on the board's short traces** (it failed on the rig's hand-wired leads): if it works, the microSD wiring stays as it is.
- **The bus switches:** BUS_EN#, and the DMARQ test after it.
- **Signal quality on the bus,** which picks the series resistors' values (4).

## The pin map

Rev 0.3's free GPIOs were 2, 32, 33 and 42-47. Rev 0.4 uses all but one:

| GPIO | Signal | Item |
|---|---|---|
| 2 | IOCS16# (IDE pin 32) | 1 |
| 32 | DASP# (IDE pin 39) | 5 |
| 33 | PDIAG# (IDE pin 34) | 5 |
| 42 | CSEL (IDE pin 28) | 5 |
| 43 | SD_PWR# (the card's power switch, low = on) | 3 |
| 44 | AUDIO_L (PWM slice 10 A) | 6 |
| 45 | AUDIO_R (PWM slice 10 B) | 6 |
| 46 | spare, unconnected | |
| 47 | PSRAM_CS# (XIP chip select 1) | 7 |

The four IDE lines take rev 0.3's four spare bus-switch channels (U6, channels 1-4). After rev 0.4, a new signal needs an I/O expander, as the HDD Synth Mk5 has.

## 1. IOCS16# (must; proven)

The ICL 386's ISA card needed it (0.20.0): without it, every 16-bit data read reached the drive as two 8-bit reads, of the data register and then the error register, so each word's high byte was wrong. `iocs16.pio` pulls it low while the data register is addressed, from the bus program's GPIO window: **GPIO 2**, the only free pin in it. It's open drain, and the host pulls it up, so no resistor. It shares `ide_dma`'s PIO block and runs only when DMA isn't offered; ISA machines never use DMA, so that costs nothing. The idea of decoding it in logic (CS0# low and DA2-0 = 000) isn't needed.

## 2. A 44-pin 2.5-inch connector (must; new)

A 2 mm 2x22 header beside the 40-pin one, in parallel on the same nets: pins 1-40 as the 40-pin header (20 is the key), 41 and 42 +5 V from the host (logic and motor), 43 GND, 44 not connected.

The Amiga 1200's IDE port is 44-pin (2.5-inch drives), and the A1200 still doesn't boot from the drive. The best guess is the 30 cm cable and the breadboard's 10-15 cm of wiring: Gayle's address and chip selects are only valid before the strobe, and change as it starts. A board that plugs into the A1200 with a short 44-pin cable removes that question, and takes its power from pins 41 and 42 (item 8). Laptops and the Atari Falcon use the same connector.

Only one of the two connectors may go to a host at a time (the silkscreen says so).

## 3. A power switch for the microSD card (must)

A P-channel MOSFET (AO3401A) on the card's 3.3 V, its gate on **GPIO 43**:
- **Pulled down by 4.7 kΩ,** so the card is powered unless the firmware drives the gate high: today's firmware works unchanged. 4.7 kΩ is under the 8.2 kΩ that RP2350 erratum E9 asks of an external pull-down (internal pull-downs stay banned).
- **Why it's a must now:** a card left in SPI mode stays there until its power is cut, so moving to SDIO needs a power cycle (29 Oct). And a warm restart has left the card stuck (SdFat code 23), cleared only by power-cycling the board.
- **The card's pull-ups (CMD, DAT0-3) move to the switched rail,** so an unpowered card isn't fed through them. Card detect's pull-up stays on 3.3 V: the switch is in the socket, not the card.
- **Inrush:** the 10 µF bulk stays on the 3.3 V side; the card's side has 1 µF and 100 nF, so turning it on doesn't pull 3.3 V down.
- **Firmware, before switching off:** set the card's pins to inputs (or low), or the card is powered through its pins.

## 4. Series resistors on the bus inputs and data lines (must; footprints, fitted with 0 Ω)

0402 footprints on **DD0-15, CS0#, CS1# and DA0-2**, between the RP2350B and the bus switches, as rev 0.3 has 33 Ω on DIOR# and DIOW# (R14, R15). Fitted with 0 Ω; values chosen on rev 0.3's bus (ATA's 22-33 Ω is the starting point).

What's been seen since:
- **The DMA soak (29 Oct):** all 16 DD lines rising together settled later than the host's strobe edge, and stronger drive (8 mA, fast slew) made it worse. Damping, not drive strength, is the lever.
- **The Amiga:** its chip selects and address lines change as the strobe starts, where ringing does most harm.

## 5. DASP#, PDIAG# and CSEL (must)

| IDE pin | Signal | GPIO | On the board |
|---|---|---|---|
| 39 | DASP# | 32 | 10 kΩ pull-up (ATA's device 0 pull-up); open drain |
| 34 | PDIAG# / CBLID | 33 | 10 kΩ pull-up; open drain |
| 28 | CSEL | 42 | 10 kΩ pull-up (ATA's device pull-up); the host grounds it for device 0 |

The pull-ups are on the RP2350B's side of the bus switch, to 3.3 V.

**Why, now:** both retro PCs' BIOSes found our device 1 without DASP#, because our board answers for both devices, so emulating two drives doesn't need these lines. They're needed to **share the cable with a real drive**, and the CD-ROM work makes that the likely set-up: the PC's own hard disk as device 0 and our CD-ROM as device 1. Without DASP# and PDIAG#, sharing is unsafe: a real device 0 that doesn't see device 1 may answer device 1's register reads, and fight our drive on the bus. CSEL picks device 0 or 1 by the cable's connector.

DASP# also lights the PC case's disk light. The host's LED resistor (from 5 V) sinks through the bus switch into the GPIO: drive it at 12 mA.

## 6. CD audio out (should; new)

A 4-pin header in CD-ROM drives' order (left, ground, ground, right), to the sound card's CD input, as real drives have. BIN/CUE images' audio tracks are left out today because the drive has no audio output; mixed-mode games play their music from them.
- **PWM on GPIO 44 and 45,** one PWM slice, each through a two-stage RC filter (1 kΩ and 10 nF, twice: about 16 kHz) and a 10 µF DC-blocking capacitor.
- **Why PWM, not an I2S DAC:** a DAC needs a PIO state machine, and the three PIO blocks are full ([pio-allocation.md](pio-allocation.md)). PWM needs none: 10 bits at about 195 kHz from the 200 MHz clock, with the 3.3 V rail's noise in it. Good enough for game music; a DAC is for a later board with an expander.
- **Firmware:** the MMC audio commands (PLAY AUDIO, READ SUB-CHANNEL's position, PAUSE/RESUME), and the BIN's audio frames streamed to PWM by DMA. A project of its own; the footprints are cheap now.
- **Check the header against a real CD-ROM drive's audio cable** before ordering: the footprint is a plain 2.54 mm 1x4.

## 7. PSRAM (should)

8 MB QSPI PSRAM (APS6404L-3SQR, SOIC-8) on the RP2350's second chip select, **GPIO 47** (its options are 0, 8, 19 and 47; only 47 is free), sharing the flash's QSPI lines, with a 10 kΩ pull-up on its chip select. About a dollar.

It unlocks a read cache (over SPI the card gives about 1.8 MB/s, where multiword DMA alone reaches 7.7 MB/s from RAM), write-back caching, and a RAM disk far larger than today's. Uses found since: a read cache for the CD-ROM, buffers for two drives, a folder disk's tables. A write-back cache needs a decision about power loss ([pcb-bom.md](pcb-bom.md), open question 6).

## 8. Power from a retro PC (should)

A CD-ROM drive lives in a 5.25-inch bay, so the board will live inside the PC, where every retro test so far was powered from a laptop's USB.
- **A Molex disk-power header** (5.08 mm, 4 pins: +12 V, GND, GND, +5 V; +12 V not connected), through a 500 mA polyfuse and a Schottky into VB, diode-ORed as rev 0.3's other inputs.
- **The 44-pin connector's +5 V** (pins 41 and 42), the same way.

With the diodes, the board can take power from several inputs at once, USB included, without feeding one into another. Rev 0.3's note "never connect the IDE adapter's 5 V to this board" was about a direct connection; it doesn't apply to a diode-ORed input.

## 9. A backup pull-up on IORDY (added 10 Nov)

R53, 10 kΩ (0402, LCSC C25744), from IORDY on the header's side of its bus switch (U5's B side, header pin 27) to +3V3, fitted. ATA puts IORDY's pull-up on the host (about 1 kΩ to 5 V), and the drive only pulls it low or lets go. A host without one leaves IORDY floating, which it may read as "not ready" for good: the likeliest reason a second NS1081 adapter (serial `0123456789ABCDE`) never detected the rev 0.3 board, where the rig's adapter did.
- **On a host that pulls up:** beside its 1 kΩ, 10 kΩ barely moves the high level, and pulling IORDY low sinks about 0.5 mA more than the 5 mA it already does through the switch.
- **Header side:** it holds IORDY high (ready) while the bus switches are off, at start-up.
- **To 3.3 V, not VB:** 3.3 V is a valid high (ATA's is 2.0 V), the host's 5 V pull-up pushes only about 0.15 mA back through it, and VB is local to the power sheet. With the board off, the host's own pull-up still holds IORDY high.
- **On the board:** between U5 and J5, above the IDE traces, at (124.7, 136.5); its +3V3 pad goes by a via to the 3V3 plane, its IORDY pad by a via and a 3.5 mm bottom-layer trace to IORDY's via at (124.71, 141.37). Placed by script on the routed board (no other trace moved); DRC as before (two silkscreen-to-edge warnings, nothing unconnected), and `check_netlist.py` now requires the pull-up on the header's side.

## 10. Silkscreen as on the FDD Synth Pro (added 10 Nov)

- **Header labels** in the FDD Synth Pro's (and the HDD Synth MK5's) style: bold, 1 mm, knocked out of a filled box (`LABEL` in `layout_pcb.py`): IDE 40-pin, IDE 44-pin (2 mm), Synth link, SWD, 5V in, Molex power, CD audio, SD card, Boot sel, Reset. Pin names (5V GND TX RX, CLK GND DIO, 12V GND GND +5V, L-G-G-R) and the notes (pull pin 20, one host only; JP1) stay plain, beside what they name. The references the labels stand for are hidden on the silkscreen (J2-J9, SW1, SW2); the fab layer keeps them.
- **On the back:** "Nostalgia by Peter Bridger" read up the left edge, as on the FDD Synth Pro (the same place on the same outline); the board's name and revision, which no longer fit on the front beside the Molex label; and two easter eggs, made by `tools/make_eggs.py` from `media/Defrag.png` (24 x 8.6 mm, upright between the two IDE headers, reading up as the credit does) and `media/ParkHeads.png` (20 x 10 mm, behind the buttons). Their black prints as silkscreen, so they show white, in reverse. Each is sized so its finest gaps print (0.15 and 0.21 mm) and placed where the back has no through-hole pad and no via within 0.3 mm.
- **Checked:** DRC as before (the two silkscreen-to-edge warnings on the USB socket, nothing unconnected, no silkscreen overlaps), parity as before (pin 20), ERC clean (0 errors, 0 warnings), `check_netlist.py` passes; the BOM and placement files are unchanged (the eggs are board-only, like the logo).

## 11. The new name: IDE Synth Bay (added 10 Nov)

The project is renamed from HDD Synth Pro to IDE Synth Bay: it serves CD-ROM drives as well as hard disks now. On the board: the logo is `LOGO_IDESynthBay` (17.2 x 15 mm, where the old one was), made by `tools/make_logo.py` from `media/IDESynthBay.png` (the discs' grey shading drops out; "BAY"'s thin bars are closed into solid letters, since 0.1 mm bars wouldn't print); the back reads "IDE Synth Bay rev 0.4"; the schematic's title blocks say IDE Synth Bay. The files keep their names (`hddsynthpro.kicad_*`, the `HDDSynthPro` library), so nothing that refers to them changes. DRC, parity and ERC as before, `check_netlist.py` passes, the BOM and placement files unchanged.

## Dropped since 29 Oct

- **The debug header:** USB-C carries the console, and has been enough for every measurement (core 1's profiles print in `s`). GPIO 46 stays spare.
- **A user button or DIP switch:** disc changes come from the HDD Synth over the activity link (`$HDM`, its OLED screens: `tools/oled_mockups`), and `HDDSYNTH.INI` picks device 0 or 1.

## No hardware needed

What the retro PCs taught that firmware covers: IORDY on ISA cards that assert chip selects without a strobe (the 23 µs release), INTRQ on ISA cards with no pull-down (driven low), choosing the bus program by itself (`dior_no_cs`, [auto-bus.md](auto-bus.md)), and the USB adapter's hangs (a rig problem).

## Already right in rev 0.3 (keep)

- **DMARQ on GPIO 31 and DMACK# on GPIO 3**, inside the bus program's window. Multiword DMA mode 2 works on the rig on those pins: 7.7 MB/s reads, 13.9 MB/s writes, 10-hour soak clean (0.17.1). The firmware tests DMARQ at start-up with a pull-up against the host's pull-down (0.16.4), so the bus switches must be on (BUS_EN#, GPIO 41) before that test.
- **The bus switches with Ioff**: they isolate the bus when the board is unpowered, so the BB48's power-order rule goes away.
- **SD card detect on GPIO 40**: the re-mount code (0.13.0) can use it instead of probing.
- **DAT0-3 on consecutive GPIOs (36-39)**, as PIO SDIO needs, and CLK/CMD on 34/35.

## Bring-up: tests owed

- [ ] **The ICL 386 with the strobe-aware IORDY watchdog** (branch `iordy-watchdog-strobes`, 0.32.3-dev.2: 0.32.1-dev.6's watchdog on today's `main`; CHANGELOG). Rev 0.3 can't run it: the 386's ISA card needs IOCS16#, which rev 0.4 wires. With `isa = on` and `ICL386MB.IMG`: MS-DOS boots and runs as on 0.32.0 (no freeze from the card's aliased chip selects, which the watchdog is for), and `s` shows `iordy_early` low. Then merge `iordy-watchdog-strobes` to `main` (the old `watchdog-strobes` can't be merged: 0.32.1 already took its commits and undid the watchdog). Why: [atapi.md](atapi.md#a-wrong-data-word-on-the-evesham-doom-quits-10-nov-2026-0320).

## Firmware for rev 0.3 itself

Done in 0.16.6: the firmware drives **BUS_EN# (GPIO 41) low** once the bus pins are set up, so JP1 can stay open, and `p` reads **SD_CD (GPIO 40)**. The first day with the board: [pcb-bringup.md](pcb-bringup.md).

## The design files (8 Nov, started)

Rev 0.4 is generated as rev 0.3 was: `hardware/tools/gen_schematic.py` writes the schematic, `layout_pcb.py` places the parts, `autoroute.py` routes them with Freerouting, and `check_netlist.py` checks the result against `src/config.h`. Progress and what's left: [hardware/README.md](../hardware/README.md).
