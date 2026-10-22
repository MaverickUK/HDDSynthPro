# Custom PCB: design targets and BOM

A starting point for the board that replaces the hand-wired prototype. It lists what the board needs and why. Part numbers are candidates: check each against its datasheet and stock before layout. Lines marked **check** are the ones I'm least sure of.

## Design targets

| Target | Why it matters for the parts |
|---|---|
| **Multiword DMA mode 2 and PIO mode 4**: 16.7 MB/s, 120 ns cycle, 70 ns strobe | The device can no longer buy time with IORDY on each word. Data must already be on the bus when the strobe comes, so the data path is buffered and fed from RAM by DMA. |
| No Ultra DMA | No source-synchronous strobes, CRC or 80-wire cable detection. Normal 40-wire cables are fine. |
| Safe in any power order | The prototype's biggest risk: the host drives the bus whenever it is powered (seen with the NS1081). Every bus pin goes through a buffer with partial-power-down (`Ioff`) protection, so an unpowered board is never fed through its pins. |
| Real hosts, not just USB adapters | Real hosts need master, slave and cable select, DASP#/PDIAG# handshaking, IOCS16# for ISA-era hosts, and ATA series termination. |
| Powered from the drive connector | A standard 4-pin power connector, plus USB-C for flashing and debugging. |
| Room to merge HDD Synth | The audio, encoder and OLED parts of the MK5 board, listed separately in section 5. |

### Timing that shapes the design

| | PIO 0 (prototype) | PIO 4 | MW DMA 2 |
|---|---|---|---|
| Cycle time | 600 ns | 120 ns | 120 ns |
| Strobe pulse | 165 ns | 70 ns | 70 ns |
| Address/CS setup to strobe (t1) | 70 ns | 25 ns | (CS valid before DMACK#) |
| Read data setup to strobe end (t5) | 50 ns | 20 ns | per spec table |
| Bus release after DIOR# (t6z) | 30 ns | 30 ns | per spec table |
| Flow control | IORDY per word | IORDY possible, but slow | DMARQ between bursts; no IORDY |

These are from ATA/ATAPI-4; check the multiword DMA rows against the spec tables before committing to the firmware design.

What these mean in practice:
- **There's no time to react within a cycle.** At 150 MHz, 70 ns is about 10 PIO cycles. The input synchroniser, buffer delays and two or three instructions use most of that.
- **Data has to be ready before the host asks for it.** The PIO program must already hold the next word in its output register before the strobe arrives, refilled by DMA from a sector buffer.
- **Strobes skip the input synchroniser.** The prototype already does this for DIOR#. It's safe because the strobes go through Schmitt-trigger input buffers, so their edges are clean.
- **The storage must keep up.** 16.7 MB/s from SD needs SDIO 4-bit at 50 MHz (25 MB/s raw). SPI mode tops out around 2–3 MB/s.
- **Faster clocks are an option.** Running the RP2350 at 200–250 MHz buys margin, but test it per board.

## 1. Signal groups and how each is buffered

All bus-side buffers run from **3.3 V**. The ATA bus uses TTL levels (inputs high above 2.0 V), so 3.3 V outputs are fine, as CompactFlash cards in True IDE mode show. The inputs tolerate 5 V hosts.

| Group | Signals | Direction (device's view) | Buffer | Termination (device side) |
|---|---|---|---|---|
| Data | DD0–DD15 | Both | 74LVC16245A, a 16-bit transceiver (DIR and OE# from the RP2350) | 33 Ω series per line (check the ATA-4 device termination table) |
| Address | DA0–DA2, CS0#, CS1# | In | 74LVC244A | — |
| Strobes | DIOR#, DIOW#, DMACK# | In | 74LVC2G17 (Schmitt) ×2; the spare input takes RESET# | Footprint for 22–33 Ω series, fitted only if ringing shows up |
| Reset | RESET# | In | Schmitt, the spare 74LVC2G17 input | — |
| Config | CSEL | In | 74LVC244A spare input | 10 kΩ pull-up (device side, per spec) |
| Flow control | DMARQ | Out, tri-state | 74LVC1G125 (OE from the RP2350) | 22 Ω series |
| Flow control | IORDY | Out, open drain | 74LVC07A (hex open drain) | — (the host pulls up) |
| Interrupt | INTRQ | Out, tri-state | 74LVC1G125 | 22 Ω series |
| Handshake | DASP#, PDIAG# | Both, open drain | 74LVC07A to drive; read back through a 74LVC244A input | 10 kΩ pull-ups per spec (check which side owns which) |
| Legacy | IOCS16# | Out, open drain | 74LVC07A | — |

**Why these parts:**
- **LVC logic** is 5 V tolerant on its inputs, and its outputs tolerate 5 V while disabled.
- **`Ioff`** means the buffer's pins go high-impedance when the board is unpowered.
- **Propagation delay is about 3–5 ns**, small against a 70 ns strobe.

**Why not the alternatives:**
- **Avoid the bus-hold variants** (74LVC**H**16245A). Bus-hold fights the host's 10 kΩ pull-down on DD7.
- **Avoid auto-direction level shifters** (TXB/TXS series). They don't cope with the bus's pull-ups and pull-downs, or with a direction that changes every cycle.

**The data transceiver must stay disabled except during this device's read cycles and DMA-in bursts.** Its DIR and OE# lines are driven by the PIO program, as side-set or as extra output pins.

## 2. Pin budget (RP2350B)

Each PIO state machine sees one 32-pin GPIO window: 0–31 or 16–47. Everything the bus programs need at speed must fit inside one window.

| Needed by PIO at speed | Pins |
|---|---|
| DD0–DD15 | 16 |
| DA0–DA2, CS0#, CS1# | 5 |
| DIOR#, DIOW#, DMACK# | 3 |
| IORDY, DMARQ | 2 |
| Data transceiver DIR, OE# | 2 |
| **Total** | **28 of 32** |

Everything else can live outside the window, read or driven by core 1 directly:

| Not needed by PIO | Pins |
|---|---|
| RESET#, INTRQ, DASP#, PDIAG#, CSEL, IOCS16#, output enables | ~7 |
| SD card (SDIO 4-bit: CLK, CMD, DAT0–3) | 6 |
| Master/slave jumper or config | 1–2 |
| HDD Synth: I2S (3), OLED I2C (2), encoder (3), buttons (2), RGB LED (1), amp enable (1), power-fail sense (2) | ~14 |

That totals about 58 GPIOs against 48. It doesn't fit without cuts:
- **Drop or multiplex** the encoder and buttons, for example onto an I2C expander such as a PCA9555.
- **Give up separate read-back pins** for DASP# and PDIAG#.

Decide this before layout. RP2350B is required either way: the QFN-80 package has 48 GPIOs, against the RP2350A's 30.

## 3. BOM: IDE drive emulator core

| Ref | Qty | Part | Candidate MPN | Package | Notes |
|---|---|---|---|---|---|
| U1 | 1 | Microcontroller, RP2350B | RP2350B | QFN-80 | 48 GPIOs; follow the RP2350 hardware design guide for the core regulator |
| L1 | 1 | Core regulator inductor, 3.3 µH | Abracon AOTA-B201610S3R3-101-T, or the BB48's PIM201610-3R3 | 2016 | Per the RP2350 design guide; placement matters |
| Y1 | 1 | Crystal, 12 MHz | Abracon ABM8-12.000MHZ-B2-T | 3.2 × 2.5 mm | With 2 load caps (check the value for the chosen crystal) |
| U2 | 1 | QSPI flash, 16 MB | Winbond W25Q128JVSIQ | SOIC-8 | Firmware, plus room for settings |
| U3 | 1 | QSPI PSRAM, 8 MB (optional) | APS6404L-3SQR-SN | SOIC-8 | On QMI CS1 (GPIO 0, 8, 19 or 47). For big sector caches, a RAM disk, or synth samples. Fit it, or not, from the start, because its CS pin is fixed |
| U4 | 1 | 16-bit transceiver, data | Nexperia 74LVC16245ADGG | TSSOP-48 | One half for DD0–7, the other for DD8–15; both DIRs tied together |
| U5 | 1 | Octal buffer, address/config inputs | 74LVC244APW | TSSOP-20 | DA0–2, CS0#, CS1#, CSEL, DASP#/PDIAG# read-back |
| U6, U7 | 2 | Dual Schmitt buffer, strobes | 74LVC2G17GW | SOT-363 | DIOR#, DIOW#, DMACK#, RESET# |
| U8 | 1 | Hex open-drain buffer | 74LVC07APW | TSSOP-14 | IORDY, DASP#, PDIAG#, IOCS16#, 2 spare |
| U9, U10 | 2 | Single tri-state buffer | 74LVC1G125GW | SOT-353 | DMARQ, INTRQ |
| RN1–RN4 | 4 | Series termination, 33 Ω, 4-way array | e.g. Yageo YC164-JR-0733RL | 1206 array | DD0–15 (check values against ATA-4) |
| R | 4 | Series termination, 22 Ω | 0402/0603 | — | DMARQ, INTRQ, plus footprints on the strobes |
| R | 4 | Pull-ups, 10 kΩ | 0402/0603 | — | CSEL, DASP#, PDIAG#, spare |
| J1 | 1 | IDE header, 40-pin, 2.54 mm, shrouded, keyed | Würth 61204021621 (**check**), or any 2×20 box header | THT | Pin 20 removed for the key |
| J2 | 1 | IDE header, 44-pin, 2.0 mm (optional, later) | 2×22 2.0 mm male header | THT/SMD | For 2.5-inch hosts (Amiga A600/A1200) |
| J3 | 1 | Master / slave / cable-select jumper | 2×3 2.54 mm header + jumper | THT | Read by GPIO (or hard-wire CSEL only) |
| J4 | 1 | Activity LED header | 1×2 2.54 mm | THT | DASP# through the open-drain buffer, with a series resistor |
| D | 1 | Activity LED + resistor | 0603 LED, ~470 Ω | — | Driven from DASP# |

### Power

| Ref | Qty | Part | Candidate MPN | Package | Notes |
|---|---|---|---|---|---|
| J5 | 1 | Drive power connector, 4-pin Mate-n-Lok | TE 350211-1 (**check**) | THT | +5 V and GND only; leave the +12 V pin unconnected |
| U11 | 1 | eFuse on +5 V | TI TPS25921A | SOT-23-8 | As on MK5: current limit and undervoltage; its fault output gives a power-fail signal |
| U12 | 1 | Power mux, drive 5 V / USB 5 V | TI TPS2116 | SOT-583 | Prefers drive power; never feeds USB back into the host or the drive rail |
| U13 | 1 | 3.3 V regulator, ≥ 600 mA | TI TPS62A02 (buck, as on the BB48) or TLV62569 | SOT-563 | The RP2350, SD card and buffers need about 150–250 mA |
| C | ~25 | Decoupling, 100 nF | 0402 | — | One per supply pin: RP2350 (per the guide), each buffer, flash, PSRAM, SD |
| C | ~6 | Bulk, 10–22 µF | 0603/0805 | — | Regulator in and out, the SD card, and the 5 V input |

### USB-C, debug and storage

| Ref | Qty | Part | Candidate MPN | Package | Notes |
|---|---|---|---|---|---|
| J6 | 1 | USB-C receptacle (USB 2.0) | GCT USB4105-GF-A | SMD | As on the BB48; 5.1 kΩ on CC1/CC2 |
| U14 | 1 | USB ESD protection | ST USBLC6-2SC6 | SOT-23-6 | On D+/D− |
| R | 2 | USB series resistors, 27 Ω | 0402 | — | Per the RP2350 design guide |
| J7 | 1 | microSD socket, push-push | Molex 503182-1852 (**check**) | SMD | Card detect wired to a GPIO |
| R | 5 | SD pull-ups, 10 kΩ | 0402 | — | CMD and DAT0–3. MK5 lesson: DAT1/DAT2 were left floating |
| R | 1 | SD CLK series termination, 22–33 Ω | 0402 | — | MK5 lesson: no series termination |
| SW1, SW2 | 2 | BOOT and RESET buttons | Any 6 mm tact or SMD | — | RESET goes to the RP2350's RUN pin; never connect the IDE RESET# to RUN |
| J8 | 1 | SWD header | Tag-Connect TC2030, or a 1×3 1.27 mm header | — | For GDB via the Debug Probe |
| J9 | 1 | Debug UART header | 1×3 2.54 mm | — | Optional |
| TP | ~8 | Test points | — | — | 3V3, 5V, GND, DIOR#, DIOW#, DMARQ, DMACK#, IORDY, for the logic analyser |

## 4. Things the prototype taught that the board must fix

| Prototype issue | Board fix |
|---|---|
| Unpowered RP2350 fed through its pins by the adapter | `Ioff` buffers on every bus line; the power-order rule disappears |
| IORDY bought time on every word, capping speed | Data pre-loaded by DMA; IORDY only on rare slow paths (register reads during PIO, SD stalls under BSY) |
| SD in SPI mode | SDIO 4-bit, pull-ups on all data lines, CLK termination, card detect |
| Hand-wired ringing on the strobes | Schmitt input buffers, short traces, ground plane, optional series resistors |
| MK5: TPS63020 PG and the card detect not connected | Connect PG and card detect to GPIOs |
| MK5: thin bulk capacitance on VSYS | Bulk capacitance sized per the supercap and amp datasheets |

## 5. Optional: the HDD Synth parts (merged board)

Carried over from the MK5 board (see the [HDDSynth_Native](https://github.com/MaverickUK/HDDSynth_Native) CLAUDE.md for its known issues):

| Qty | Part | Candidate MPN | Notes |
|---|---|---|---|
| 1 | I2S amplifier, mono, 3 W | MAX98357A | As MK5. With SD_MODE on a GPIO |
| 1 | Speaker connector | JST PH 2-pin | — |
| 1 | OLED header | 1×4 2.54 mm (SSD1306 on I2C) | Or a JST SH 4-pin (Qwiic) |
| 1 | Rotary encoder with push | Alps EC11 series | Pins are tight: consider an I2C expander |
| 1 | Action button | 6 mm tact | — |
| 1 | RGB LED | WS2812B-2020, or a discrete RGB LED | Activity colour |
| 1 | Supercap backup boost | TI TPS61094 | As MK5. Revisit: the MK5 supercap failed after one deep discharge. Consider balancing, or a cut-off before full drain |
| 1 | Supercap | 1 F, 5.5 V | Prefer a part with a published ESR; check cell balancing |

## Open questions before layout

1. **Pin plan.** Fit the about 58 wanted GPIOs into 48, with the 28 fast ones inside one PIO window (section 2).
2. **Termination values.** Confirm against the ATA/ATAPI-4 or ATA/ATAPI-5 device termination table.
3. **Multiword DMA timings.** Firmware feasibility of MW DMA mode 2 at 150 MHz versus 200+ MHz. Prove it in `host/hddproto/pio_sim.py` first, as was done for PIO mode 0.
4. **PSRAM: fit or not.** Its chip-select is a fixed QMI CS1 pin, so decide now.
5. **Form factor.** 3.5-inch mounting holes and power connector, or 2.5-inch with the 44-pin header (power on pins 41–43).
6. **Which hosts must work.** USB bridges, a 486 with an ISA controller (needs IOCS16#), a PCI chipset, the Amiga's Gayle.
