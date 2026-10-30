# Wiring: IDE header to Olimex RP2350-PICO2-BB48

The pin map lives in [src/config.h](../src/config.h); this table must match it.

[wiring.html](wiring.html) shows the same connections as an interactive diagram. Open it in a browser: GitHub shows HTML files as source. It includes a soldering checklist, and its ticks are saved in that browser only.

## Why these pins

- **GPIO 0–1 are off the IDE bus, GPIO 2 carries only IOCS16# and GPIO 3 only DMACK#.** The BB48 pulls GPIO 2 and 3 up to 3.3 V through 2.2 kΩ (R17/R18, for the UEXT and Qwiic I2C connectors). Both pull-ups are harmless here: DMACK# is driven by the host, and IOCS16# is open drain, which the host also pulls up (added 30 Oct 2026 for 16-bit ISA cards, `iocs16.pio`). DMARQ is on GPIO 31 (multiword DMA, [dma.md](dma.md); added 29 Oct 2026, as on the PCB). GPIO 0 (EXT1 pin 4) is UART0 TX for the activity link to an HDD Synth, and GPIO 1 (EXT1 pin 5) is UART0 RX for its reply (the synth's `$HDM`, since 0.19.0; pulled up inside, so it can stay unconnected); wiring in [activity-link.md](activity-link.md).
- **GPIO 25 is off the bus.** It drives the board's LED, which the drive build uses as its activity light.
- **DD0–15, DA0–2, CS0#, CS1# are on 21 consecutive pins (GPIO 4–24)**, so one PIO read captures a whole register access.
- **Everything is within GPIO 0–31**, the range one PIO block can reach.
- **These parts of the variant file can be ignored.** The core's variant file names GPIO 8 as PSRAM chip-select and GPIO 9–11 and 24 as SD lines. Those parts are only fitted on the BB48**R**; on the plain BB48 the pins are free.

BB48 header pins: GPIO 0–23 are on EXT1 pins 4–27, and GPIO 24–47 are on EXT2 pins 4–27. GND is EXT1 pin 2 and EXT2 pin 3.

## Connections

| IDE pin | Signal | Direction (drive's view) | GPIO | BB48 pin |
|---|---|---|---|---|
| 1 | RESET# | in | 30 | EXT2-10 |
| 3 | DD7 | in/out | 11 | EXT1-15 |
| 4 | DD8 | in/out | 12 | EXT1-16 |
| 5 | DD6 | in/out | 10 | EXT1-14 |
| 6 | DD9 | in/out | 13 | EXT1-17 |
| 7 | DD5 | in/out | 9 | EXT1-13 |
| 8 | DD10 | in/out | 14 | EXT1-18 |
| 9 | DD4 | in/out | 8 | EXT1-12 |
| 10 | DD11 | in/out | 15 | EXT1-19 |
| 11 | DD3 | in/out | 7 | EXT1-11 |
| 12 | DD12 | in/out | 16 | EXT1-20 |
| 13 | DD2 | in/out | 6 | EXT1-10 |
| 14 | DD13 | in/out | 17 | EXT1-21 |
| 15 | DD1 | in/out | 5 | EXT1-9 |
| 16 | DD14 | in/out | 18 | EXT1-22 |
| 17 | DD0 | in/out | 4 | EXT1-8 |
| 18 | DD15 | in/out | 19 | EXT1-23 |
| 21 | DMARQ | out, only during a DMA command | 31 | EXT2-11 |
| 23 | DIOW# | in | 27 | EXT2-7 |
| 25 | DIOR# | in | 26 | EXT2-6 |
| 27 | IORDY | out, open drain | 28 | EXT2-8 |
| 29 | DMACK# | in | 3 | EXT1-7 |
| 31 | INTRQ | out | 29 | EXT2-9 |
| 32 | IOCS16# | out, open drain | 2 | EXT1-6 |
| 33 | DA1 | in | 21 | EXT1-25 |
| 35 | DA0 | in | 20 | EXT1-24 |
| 36 | DA2 | in | 22 | EXT1-26 |
| 37 | CS0# (ATA "CS1FX-") | in | 23 | EXT1-27 |
| 38 | CS1# (ATA "CS3FX-") | in | 24 | EXT2-4 |
| 2, 19, 22, 24, 26, 30, 40 | GND | | | EXT1-2, EXT2-3 |

**Not connected:**
- 20 (key)
- 28 CSEL
- 34 PDIAG#
- 39 DASP#

Never connect the adapter's +5 V (its Molex power) to the BB48. The two boards share ground only.

## Notes

- **Grounds:** wire at least three of the IDE grounds, spread along the connector, to the BB48's ground.
- **Series resistors:** fit 33 Ω in DIOR# and DIOW#, close to the BB48, to damp ringing on the strobes. Keep the wires short (under 15 cm if possible).
- **IORDY is open drain.** The host pulls it up to 5 V through about 1 kΩ. The firmware only ever pulls it low or releases it, never drives it high.
- **Pull-ups and pull-downs come from the host side:**
  - IORDY: pulled up
  - INTRQ: pulled down, about 10 kΩ
  - DD7: pulled down, about 10 kΩ, so an empty bus reads a status with BSY clear

  The firmware never enables internal pull-downs (RP2350 erratum E9).
- **Power order:** power the BB48 (USB-C) before plugging in the adapter's USB, and unplug the adapter's USB before unplugging the BB48. The RP2350's pins are only 5 V tolerant while it is powered. Flashing is fine: USB power stays on throughout.
- **The adapter drives the bus by itself.** Whenever the NS1081 adapter is powered, it drives the IDE signal lines, whether or not the drive end is.
  - This was seen on 22 Oct 2026. With a CF-to-IDE adapter's own power switched off, its LEDs stayed lit, fed through the signal lines. The card couldn't be read.
  - An unpowered BB48 on the cable would be fed the same way, through its pins' protection diodes, at up to 5 V. This is why the power-order rule matters.
  - The adapter is powered by its USB cable; its brick only powers a drive plugged into it. Switching its hub port off with `uhubctl` does not cut its power (seen on 23 Oct 2026), so "adapter off" means its USB cable unplugged.

## microSD breakout (slice 5)

The Adafruit microSD breakout (SPI or SDIO, 3 V only), wired so the same leads serve SPI now and 4-bit SDIO later: CLK, CMD and DAT0 are on SPI0's SCK, TX and RX pins, DAT3 is the SPI chip select, and DAT0–DAT3 are consecutive, as PIO SDIO needs. The pin map is in [src/config.h](../src/config.h); [wiring-sd.html](wiring-sd.html) is the interactive version.

| Breakout pin | Signal | GPIO | BB48 pin |
|---|---|---|---|
| 3V | 3.3 V supply | | EXT1-1 (+3.3V) |
| GND | Ground | | EXT2-3 |
| CLK | SPI0 SCK | 34 | EXT2-14 |
| CMD | SPI0 TX (card DI) | 35 | EXT2-15 |
| DAT0 | SPI0 RX (card DO) | 36 | EXT2-16 |
| DAT1 | SDIO only | 37 | EXT2-17 |
| DAT2 | SDIO only | 38 | EXT2-18 |
| DAT3 | SPI chip select | 39 | EXT2-19 |
| DET | card detect | | not connected |

- **Pull-ups:** the breakout has none. Fit 10 kΩ from CMD and DAT0–DAT3 to 3.3 V, close to the breakout. CLK needs none.
- **Power:** from the BB48's +3.3V only, never VBUS or VSYS (5 V).
- **SPI0's default pins on this board are GPIO 4–7, which are IDE data lines.** The firmware refuses to start the card if SPI0 rejects the SD pins, rather than fall back to them.

## Wiring check

The full order for hardware day, including the solder-bridge test (console `x`), is in [bringup.md](bringup.md).

With the adapter powered and idle, the console's `p` command should show:
- DIOR#, DIOW#, CS0#, CS1# and RESET# high
- IORDY high (host pull-up)
- INTRQ low
- DD7 low; the other DD lines may float
