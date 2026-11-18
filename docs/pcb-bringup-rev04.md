# PCB bring-up: rev 0.4

The first day with rev 0.4 ([pcb-rev04.md](pcb-rev04.md), ordered 10 Nov 2026). Rev 0.3's checklist ([pcb-bringup.md](pcb-bringup.md)) still applies, and steps 1-2 here repeat its essentials. The rest is what's new: the board tells itself apart from rev 0.3, the card's power switch, DASP#, PDIAG# and CSEL, IOCS16#, the 44-pin header, the Molex input and IORDY's backup pull-up. In order: each step either passes or says what to fix before the next.

You'll need what rev 0.3 needed (a multimeter with a continuity beeper, USB-C, the USB-IDE adapter, a 40-pin IDE cable, the microSD card), and firmware from branch `rev04-firmware` (0.35.1-dev.1) or later: older firmware runs rev 0.4 as if it were rev 0.3, with the new lines left alone.

**What the firmware does with rev 0.4's lines:**
- **Tells the boards apart** at start-up by SD_PWR# (GPIO 43): rev 0.4 pulls it down (4.7 kΩ), rev 0.3 and the BB48 leave it unconnected. `s` says `board=rev0.4` or `board=rev0.3/bb48`. Until it has seen rev 0.4, it touches none of the new lines.
- **The card's power (SD_PWR#):** if a card that's in won't start over SDIO, the drive releases the card's lines, switches its power off for 250 ms, on again, and tries once more, before falling back to SPI. `s`'s `card_power_cycles` counts them.
- **DASP#** lights with the activity LED (open drain, 12 mA, never driven high): the PC case's disk light.
- **PDIAG# and CSEL** are only read, for `p`. Sharing the cable with a real drive (the reason they're wired) needs the bus programs to stay off the bus for the other device: later work.
- **Not used yet:** CD audio (GPIO 44-45) and the PSRAM (GPIO 47).

## 1. Before any power

- [ ] **Look it over** as rev 0.3: the four bus switches and the RP2350B the right way round, no bridges on the QFN-80 (rev 0.4's mask has one opening per row of pads: `check_mask_webs.py`), the 40-pin header's key at pin 20, and now the 44-pin header (2 mm), the PSRAM (SOIC-8), the card's MOSFET (AO3401A) and the Molex header.
- [ ] **No shorts on the supplies:** 3V3, VB and the Molex +5 V to GND well above 100 Ω. The Molex's +12 V pin goes nowhere.
- [ ] **JP1 open.** No IDE cable yet.

## 2. First power: USB-C only

- [ ] The power LED lights, 3V3 reads 3.3 V, the board shows as **RP2350 Boot**. Flash `pcb_drive`.
- [ ] `h`: `READY HDDSynthPro <version> slice3`.
- [ ] **`s`: `board=rev0.4`.** If it says `rev0.3/bb48`, SD_PWR# isn't reading low: check the 4.7 kΩ pull-down on GPIO 43 and that the pin reaches the MOSFET's gate. The firmware then leaves every new line alone, so the rest of this list can't run.
- [ ] `p`: `BUS_EN#=0`, and a new line `PINS DASP#=1 PDIAG#=1 CSEL=1 SD_PWR#=0` (no cable: the board's pull-ups hold the three IDE lines high; SD_PWR# low, the card powered).
- [ ] **Solder-bridge test**, cable unplugged: `x` then `Y`, `SHORTTEST OK`. It checks the bus pins, not yet GPIO 2, 32, 33 and 42: check those four for shorts to their neighbours with the meter.

## 3. The microSD card and its power switch

- [ ] Card out `SD_CD=1`, in `SD_CD=0`, as rev 0.3.
- [ ] `S`: `SD ok ... bus=sdio`. `s`: `card_power_cycles=0` (the card started first time).
- [ ] **The switch:** measure the card's rail (the socket's VDD) with a card in: 3.3 V. With a scope or a meter on it, restart with a card left in SPI mode (run `bb48_drive` once, then flash `pcb_drive` again without taking the card out): the rail drops for about 250 ms, then `S` shows `bus=sdio` with no SPI warning and `s` shows `card_power_cycles=1`. On rev 0.3 the same card stays on SPI until it's taken out.
- [ ] **How long off is enough:** the firmware waits 250 ms (`board::CARD_OFF_MS`), a guess from the rail's 1.1 µF and a card idling at about 100 µA. With a scope, see how long the rail takes to fall below 0.5 V with the card idle; if it's well under 250 ms, the wait can shrink (the drive waits that long at boot only when a card won't start).
- [ ] **Nothing powers the card through its lines:** with the card's power off (hold the restart in step 3's state, or drive SD_PWR# high from a debug build), the rail should read near 0 V, not 1-2 V. If it doesn't, a pull-up is still on the 3.3 V side, or a line is still driven.

## 4. Connect the adapter (40-pin)

- [ ] As rev 0.3's step 4: `p`'s bus levels, `S`'s `CONFIG dmarq=wired`, and the adapter detects the drive.
- [ ] `p`'s new line with the cable in: **CSEL** reads what the cable says (0 if the host grounds pin 28: most 40-pin cables and adapters do, for device 0). **PDIAG#** and **DASP#** read 1 (no other drive on the cable pulls them).
- [ ] **IORDY's backup pull-up (R53):** with the adapter's cable in, IORDY reads 1 in `p`, as before. With the cable out, header pin 27 reads 3.3 V to GND on the meter (rev 0.3: floating).

## 5. DASP#: the case's disk light

- [ ] On the USB adapter there's no light to watch: with a meter or scope on header pin 39, run a read (`-m speed`): it goes low while commands run and for `LED_MIN_MS` after, as the board's LED does.
- [ ] **On a retro PC** with its case's HDD LED on the motherboard's header: the light follows the drive's activity. If it's lit all the time or never, check pin 39's route through the bus switch, and that the LED's resistor is to 5 V (the host's side), not to ground.

## 6. The drive's tests

- [ ] Rev 0.3's steps 5-7 (slices 1-5, `-m speed`, multiword DMA, the SD image and SDIO), and `-m card` with the card pulled and put back. Speeds as rev 0.3's within a few percent: the 0 Ω series resistors change nothing yet.
- [ ] **A card soak**, an hour at least (`-m cardsoak` on `HDDBENCH.IMG`, `J`): 0 wrong, `card_power_cycles` as at the start.

## 7. IOCS16# and the ICL 386

- [ ] `isa = on`, `ICL386MB.IMG`, the ICL 386's ISA card: MS-DOS 5 boots, and reads every data word right (without IOCS16# each word's high byte was wrong, 0.20.0).
- [ ] **The strobe-aware IORDY watchdog** (branch `iordy-watchdog-strobes`): the test [pcb-rev04.md](pcb-rev04.md#bring-up-tests-owed) owes, then merge that branch.

## 8. The 44-pin header and the Molex input

- [ ] **44-pin:** only one header to a host at a time. On a 44-pin host (the Amiga 1200, a laptop) with a short cable: the board powers from pins 41-42 (USB-C out), the power LED lights and the drive is detected.
- [ ] **Molex:** the board powers from a PC's disk-power plug alone, and from Molex and USB-C together, with neither feeding the other (the diodes: VBUS reads no more than USB's 5 V with Molex in and the USB cable unplugged from the PC).
- [ ] **The Amiga 1200** from the 44-pin header with a short cable: does it boot now (`iordy = off`)? That was the reason for the header.

## Not on this list yet

- **CD audio** (GPIO 44-45, PWM): no firmware yet. With a debug build putting a tone out, check the header's order against a real CD-ROM drive's audio cable (L, G, G, R).
- **The PSRAM** (GPIO 47, QSPI chip select 1): no firmware yet. Arduino-Pico can map PSRAM at start-up; a first test is reading back a pattern from all 8 MB.
- **Sharing the cable with a real drive** (DASP#, PDIAG# and CSEL in earnest): needs the drive to answer only as one device.

## If something fails

| Symptom | Look at |
|---|---|
| `board=rev0.3/bb48` on rev 0.4 | GPIO 43's 4.7 kΩ pull-down and its route to the MOSFET's gate |
| `bus=spi` with a warning, and `card_power_cycles=1` | The power cycle didn't wake the card: the rail didn't fall far enough (lengthen `CARD_OFF_MS`), or something feeds the card while it's off (step 3) |
| The card's rail near 1-2 V with the power off | A pull-up left on the 3.3 V side, or a card line driven |
| No case light on a retro PC | Header pin 39, its bus-switch channel, and the LED's polarity |
| `CSEL` doesn't follow the cable | Header pin 28 and its bus-switch channel; the pull-up is on the RP2350's side |
