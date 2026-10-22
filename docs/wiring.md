# Wiring: IDE header to Olimex RP2350-PICO2-BB48

The pin map lives in [src/config.h](../src/config.h); this table must match it.

## Why these pins

- **GPIO 0–3 are not used.** The BB48 pulls GPIO 2 and 3 up to 3.3 V through 2.2 kΩ (R17/R18, for the UEXT and Qwiic I2C connectors). GPIO 0/1 stay free for a debug UART.
- **GPIO 25 is not used.** It drives the board's LED.
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
| 23 | DIOW# | in | 27 | EXT2-7 |
| 25 | DIOR# | in | 26 | EXT2-6 |
| 27 | IORDY | out, open drain | 28 | EXT2-8 |
| 31 | INTRQ | out | 29 | EXT2-9 |
| 33 | DA1 | in | 21 | EXT1-25 |
| 35 | DA0 | in | 20 | EXT1-24 |
| 36 | DA2 | in | 22 | EXT1-26 |
| 37 | CS0# (ATA "CS1FX-") | in | 23 | EXT1-27 |
| 38 | CS1# (ATA "CS3FX-") | in | 24 | EXT2-4 |
| 2, 19, 22, 24, 26, 30, 40 | GND | | | EXT1-2, EXT2-3 |

**Not connected:**
- 20 (key)
- 21 DMARQ
- 28 CSEL
- 29 DMACK#
- 32 IOCS16#
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
- **Power order:** power the BB48 (USB-C) before switching on the adapter's brick, and switch the brick off before unplugging the BB48. The RP2350's pins are only 5 V tolerant while it is powered. Flashing is fine: USB power stays on throughout.

## Wiring check

With the adapter powered and idle, the console's `p` command should show:
- DIOR#, DIOW#, CS0#, CS1# and RESET# high
- IORDY high (host pull-up)
- INTRQ low
- DD7 low; the other DD lines may float
