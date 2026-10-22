# Custom PCB: design targets and BOM

A starting point for the board that replaces the hand-wired prototype. It lists what the board needs and why. Part numbers are candidates: check each against its datasheet and stock before layout. Lines marked **check** are the ones I'm least sure of.

## Decisions so far

| Decision (22 Oct 2026) | Consequence |
|---|---|
| **Speed: up to multiword DMA mode 2 and PIO mode 4**, not Ultra DMA | Data pre-loaded by DMA, buffered bus, SDIO 4-bit storage (see "Timing" below) |
| **Disk images live on the SD card, not in flash** | Flash holds the firmware only. The SD card is the hot path: SDIO 4-bit and a fast card; PSRAM as a write cache is recommended |
| **Slow controls go on an I2C expander** | Encoder, buttons, jumpers, card detect, the activity and handshake lines all move off the GPIOs, which closes the pin budget (section 2) |
| **Work on as many hosts as practical** | IOCS16#, IORDY-less register reads, master/slave/cable select with a real second drive, both 40- and 44-pin connectors, and optional power from the IDE cable (section 4) |

## Design targets

| Target | Why it matters for the parts |
|---|---|
| **Multiword DMA mode 2 and PIO mode 4**: 16.7 MB/s, 120 ns cycle, 70 ns strobe | The device can no longer buy time with IORDY on each word. Data must already be on the bus when the strobe comes, so the data path is buffered and fed from RAM by DMA. |
| No Ultra DMA | No source-synchronous strobes, CRC or 80-wire cable detection. Normal 40-wire cables are fine. |
| Safe in any power order | The prototype's biggest risk: the host drives the bus whenever it is powered (seen with the NS1081). Every bus pin goes through a buffer with partial-power-down (`Ioff`) protection, so an unpowered board is never fed through its pins. |
| As many hosts as practical | See the host table in section 4. |
| Powered from the host | A 4-pin power connector (3.5-inch hosts), the 44-pin header's power pins (2.5-inch hosts), or optionally IDE pin 20 (DOM-style). USB-C for flashing and debugging. |
| Room to merge HDD Synth | The audio and OLED parts of the MK5 board are listed in section 6. |

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
- **Register reads are answered from a table, not by core 1.** The same fast path serves hosts that never wait on IORDY (section 4): the PIO program reads each register's value from a RAM table, by DMA, that core 1 keeps current.
- **Faster clocks are an option.** Running the RP2350 at 200–250 MHz buys margin, but test it per board.

### Storage: images on the SD card

- **The card must sustain the bus rate.** 16.7 MB/s needs SDIO 4-bit at 50 MHz (25 MB/s raw). SPI mode tops out around 2–3 MB/s. Use a card rated A1 or A2, since hosts read and write in small random blocks, not long streams.
- **Writes stall.** SD cards pause for up to a few hundred milliseconds now and then while they manage their flash. The drive shows BSY meanwhile, which hosts accept (their timeouts are seconds). PSRAM (U3) as a write-back cache hides most of these pauses.
  - The supercap backup flushes the cache on power loss.
  - So the cache and the supercap go together: either both or neither.
- **Images are contiguous files on FAT32 or exFAT.** exFAT is needed for images over 4 GB. The firmware maps LBA straight to card sectors, so no file-system work happens per access.
- **Card removal while running** is detected (card detect on the expander) and reported as a media error. It is not a supported way to swap disks.
- **Flash holds the firmware only.** 4 MB (W25Q32JV) is plenty; 16 MB costs pennies more and leaves room for a recovery image.

## 1. Signal groups and how each is buffered

All bus-side buffers run from **3.3 V**. The ATA bus uses TTL levels (inputs high above 2.0 V), so 3.3 V outputs are fine, as CompactFlash cards in True IDE mode show. The inputs tolerate 5 V hosts.

| Group | Signals | Direction (device's view) | Buffer | Driven by | Termination (device side) |
|---|---|---|---|---|---|
| Data | DD0–DD15 | Both | 74LVC16245A, a 16-bit transceiver (DIR and OE# from PIO) | PIO | 33 Ω series per line (check the ATA-4 device termination table) |
| Address | DA0–DA2, CS0#, CS1# | In | 74LVC244A | PIO | — |
| Strobes | DIOR#, DIOW#, DMACK# | In | 74LVC2G17 (Schmitt) ×2; the spare input takes RESET# | PIO | Footprint for 22–33 Ω series, fitted only if ringing shows up |
| Reset | RESET# | In | Schmitt, the spare 74LVC2G17 input | Core 1 | — |
| Flow control | DMARQ | Out, tri-state | 74LVC1G125 (data and OE from PIO) | PIO | 22 Ω series |
| Flow control | IORDY | Out, open drain | 74LVC07A | PIO | — (the host pulls up) |
| Legacy | IOCS16# | Out, open drain | 74LVC07A | PIO, which asserts it when the data register is addressed (see section 4) | — |
| Interrupt | INTRQ | Out, tri-state | 74LVC1G125 (input tied high; OE from core 1) | Core 1 | 22 Ω series |
| Handshake | DASP#, PDIAG# | Both, open drain | 74LVC07A to drive; read back through the 74LVC244A | Expander (slow: milliseconds) | 10 kΩ pull-ups per spec (check which side owns which) |
| Config | CSEL | In | 74LVC244A | Expander | 10 kΩ pull-up |

**Why these parts:**
- **LVC logic** is 5 V tolerant on its inputs, and its outputs tolerate 5 V while disabled.
- **`Ioff`** means the buffer's pins go high-impedance when the board is unpowered.
- **Propagation delay is about 3–5 ns**, small against a 70 ns strobe.
- **Drive strength is 24 mA**, plenty for long ribbons on old ISA controllers.

**Why not the alternatives:**
- **Avoid the bus-hold variants** (74LVC**H**16245A). Bus-hold fights the host's 10 kΩ pull-down on DD7.
- **Avoid auto-direction level shifters** (TXB/TXS series). They don't cope with the bus's pull-ups and pull-downs, or with a direction that changes every cycle.

**When not selected, the board must vanish from the bus.** The data transceiver, DMARQ, INTRQ and IOCS16# stay disabled unless this device is selected and in the right cycle. This is what lets it share a cable with a real drive.

## 2. Pin budget (RP2350B, 48 GPIOs)

Each PIO state machine sees one 32-pin GPIO window: 0–31 or 16–47. The fast bus signals must all fit inside one window.

| Group | Pins | Where |
|---|---|---|
| DD0–DD15 | 16 | PIO window |
| DA0–DA2, CS0#, CS1# | 5 | PIO window |
| DIOR#, DIOW#, DMACK# | 3 | PIO window |
| IORDY, DMARQ, DMARQ OE | 3 | PIO window |
| IOCS16# | 1 | PIO window |
| Data transceiver DIR, OE# | 2 | PIO window |
| **PIO window subtotal** | **30 of 32** | |
| RESET#, INTRQ (OE) | 2 | Core 1 |
| SD card, SDIO 4-bit (CLK, CMD, DAT0–3) | 6 | A second PIO block |
| PSRAM chip-select (QMI CS1: GPIO 0, 8, 19 or 47) | 1 | Fixed pin |
| I2C (SDA, SCL) + expander interrupt | 3 | Shared by the expander, LED driver and OLED |
| I2S to the amp (BCLK, LRCLK, DATA) | 3 | HDD Synth |
| eFuse fault (power-fail interrupt) | 1 | Direct: it must react within microseconds |
| Supercap voltage (ADC) | 1 | ADC pins are GPIO 40–47 on the RP2350B |
| **Total** | **47 of 48** | 1 spare |

**On the 16-pin I2C expander:**

| Group | Pins |
|---|---|
| Encoder A, B, push | 3 |
| Action button | 1 |
| Master/slave/cable-select jumpers | 2 |
| CSEL read | 1 |
| DASP# drive + read-back | 2 |
| PDIAG# drive + read-back | 2 |
| SD card detect | 1 |
| Amp SD_MODE | 1 |
| 3.3 V regulator power-good | 1 |
| Spare | 2 |

**Layout constraints:**
- Keep the 30 window pins on consecutive GPIOs where possible. The PIO programs read DD, DA and CS# in one go, as on the prototype.
- Keep them clear of GPIO 40–47 (the ADC pins).
- The encoder through the expander is fine. Its interrupt line flags each change, and a 400 kHz read takes about 50 µs, far faster than a hand turns it.

## 3. BOM: IDE drive emulator core

| Ref | Qty | Part | Candidate MPN | Package | Notes |
|---|---|---|---|---|---|
| U1 | 1 | Microcontroller, RP2350B | RP2350B | QFN-80 | 48 GPIOs; follow the RP2350 hardware design guide for the core regulator |
| L1 | 1 | Core regulator inductor, 3.3 µH | Abracon AOTA-B201610S3R3-101-T, or the BB48's PIM201610-3R3 | 2016 | Per the RP2350 design guide; placement matters |
| Y1 | 1 | Crystal, 12 MHz | Abracon ABM8-12.000MHZ-B2-T | 3.2 × 2.5 mm | With 2 load caps (check the value for the chosen crystal) |
| U2 | 1 | QSPI flash, firmware only | Winbond W25Q32JVSSIQ (4 MB) or W25Q128JVSIQ (16 MB) | SOIC-8 | Images are on the SD card |
| U3 | 1 | QSPI PSRAM, 8 MB | APS6404L-3SQR-SN | SOIC-8 | Write cache for SD stalls, plus big read buffers. Recommended; its CS pin is fixed |
| U4 | 1 | 16-bit transceiver, data | Nexperia 74LVC16245ADGG | TSSOP-48 | One half for DD0–7, the other for DD8–15; both DIRs tied together |
| U5 | 1 | Octal buffer, address/config inputs | 74LVC244APW | TSSOP-20 | DA0–2, CS0#, CS1#, CSEL, DASP#/PDIAG# read-back |
| U6, U7 | 2 | Dual Schmitt buffer, strobes | 74LVC2G17GW | SOT-363 | DIOR#, DIOW#, DMACK#, RESET# |
| U8 | 1 | Hex open-drain buffer | 74LVC07APW | TSSOP-14 | IORDY, IOCS16#, DASP#, PDIAG#, 2 spare |
| U9, U10 | 2 | Single tri-state buffer | 74LVC1G125GW | SOT-353 | DMARQ, INTRQ |
| U15 | 1 | I2C GPIO expander, 16-bit, with interrupt | TI TCA9555PWR (or NXP PCA9555) | TSSOP-24 | Encoder, buttons, jumpers, card detect, DASP/PDIAG, amp enable, PG |
| R | 2 | I2C pull-ups, 4.7 kΩ | 0402 | — | SDA, SCL |
| R | 1 | Expander interrupt pull-up, 10 kΩ | 0402 | — | Open-drain INT# |
| RN1–RN4 | 4 | Series termination, 33 Ω, 4-way array | e.g. Yageo YC164-JR-0733RL | 1206 array | DD0–15 (check values against ATA-4) |
| R | 4 | Series termination, 22 Ω | 0402/0603 | — | DMARQ, INTRQ, plus footprints on the strobes |
| R | 4 | Pull-ups, 10 kΩ | 0402/0603 | — | CSEL, DASP#, PDIAG#, spare |
| J1 | 1 | IDE header, 40-pin, 2.54 mm, shrouded, keyed | Würth 61204021621 (**check**), or any 2×20 box header | THT | 3.5-inch hosts. Pin 20 removed for the key, unless the pin-20 power option is used |
| J2 | 1 | IDE header, 44-pin, 2.0 mm | 2×22 2.0 mm male header | THT/SMD | 2.5-inch hosts (Amiga A600/A1200, laptops). Pins 41–42 +5 V, 43 GND. Fit J1 or J2, not both |
| J3 | 1 | Master / slave / cable-select jumpers | 2×3 2.54 mm header + jumper | THT | Read through the expander |
| J4 | 1 | Activity LED header | 1×2 2.54 mm | THT | DASP# through the open-drain buffer, with a series resistor |
| D | 1 | Activity LED + resistor | 0603 LED, ~470 Ω | — | Driven from DASP# |

### Power

| Ref | Qty | Part | Candidate MPN | Package | Notes |
|---|---|---|---|---|---|
| J5 | 1 | Drive power connector, 4-pin Mate-n-Lok | TE 350211-1 (**check**) | THT | +5 V and GND only; leave the +12 V pin unconnected |
| JP1 | 1 | Pin-20 power jumper (DOM-style), default open | 1×2 2.54 mm + jumper | THT | Some hosts and adapters put +5 V on IDE pin 20. Open by default: on hosts that don't, pin 20 is the key and must stay unconnected |
| F1 | 1 | Polyfuse on the pin-20 / 44-pin power input, ~500 mA hold | e.g. Littelfuse 0805L050 (**check**) | 0805 | Protects the host's connector pin |
| U11 | 1 | eFuse on +5 V | TI TPS25921A | SOT-23-8 | As on MK5: current limit and undervoltage; its fault output gives a power-fail signal |
| U12 | 1 | Power mux, host 5 V / USB 5 V | TI TPS2116 | SOT-583 | Prefers host power; never feeds USB back into the host or the drive rail |
| D | 2 | Schottky OR-ing of the host power inputs | e.g. SS14, or an ideal-diode IC | SMA | Molex, pin 20 and the 44-pin header can't fight each other |
| U13 | 1 | 3.3 V regulator, ≥ 600 mA | TI TPS62A02 (buck, as on the BB48) or TLV62569 | SOT-563 | The RP2350, SD card, PSRAM and buffers need about 200–300 mA |
| C | ~28 | Decoupling, 100 nF | 0402 | — | One per supply pin: RP2350 (per the guide), each buffer, flash, PSRAM, SD, expander |
| C | ~6 | Bulk, 10–22 µF | 0603/0805 | — | Regulator in and out, the SD card, and the 5 V input |

### USB-C, debug and storage

| Ref | Qty | Part | Candidate MPN | Package | Notes |
|---|---|---|---|---|---|
| J6 | 1 | USB-C receptacle (USB 2.0) | GCT USB4105-GF-A | SMD | As on the BB48; 5.1 kΩ on CC1/CC2 |
| U14 | 1 | USB ESD protection | ST USBLC6-2SC6 | SOT-23-6 | On D+/D− |
| R | 2 | USB series resistors, 27 Ω | 0402 | — | Per the RP2350 design guide |
| J7 | 1 | microSD socket, push-push, with card detect | Molex 503182-1852 (**check**) | SMD | Disk images. Card detect goes to the expander |
| R | 5 | SD pull-ups, 10 kΩ | 0402 | — | CMD and DAT0–3. MK5 lesson: DAT1/DAT2 were left floating |
| R | 1 | SD CLK series termination, 22–33 Ω | 0402 | — | MK5 lesson: no series termination. Keep SDIO traces short and matched |
| SW1, SW2 | 2 | BOOT and RESET buttons | Any 6 mm tact or SMD | — | RESET goes to the RP2350's RUN pin; never connect the IDE RESET# to RUN |
| J8 | 1 | SWD header | Tag-Connect TC2030, or a 1×3 1.27 mm header | — | For GDB via the Debug Probe |
| J9 | 1 | Debug UART header | 1×3 2.54 mm | — | Optional |
| TP | ~8 | Test points | — | — | 3V3, 5V, GND, DIOR#, DIOW#, DMARQ, DMACK#, IORDY, for the logic analyser |

## 4. Host compatibility

What each kind of host needs, and where that's handled. **HW** means a board feature in this BOM; **FW** means firmware, with no extra parts.

| Host | What it needs | Covered by |
|---|---|---|
| USB-IDE bridges (NS1081 and others) | PIO or DMA, LBA, a sensible IDENTIFY | FW (the prototype's path) |
| Early AT / ISA (286, 386), ATA-1 era controllers | CHS only, often no IORDY wired, IOCS16# for 16-bit data, PIO 0–2 | **HW:** IOCS16# from PIO. **FW:** register reads answered from the PIO table with no IORDY wait; IDENTIFY with IORDY not required; CHS geometry from the image |
| 486, VLB and ISA multi-I/O cards | PIO 0–4, often LBA, BIOS geometry limits (504 MB, 8.4 GB) | **FW:** per-image geometry (settings file or VHD footer); report capacity within the BIOS limits the image is made for |
| PCI chipsets (Intel PIIX and others) | Multiword DMA 0–2, SET FEATURES transfer modes, bus mastering | **HW:** DMARQ/DMACK#. **FW:** MW DMA path |
| Amiga A600/A1200 (Gayle), Atari Falcon | 44-pin 2.5-inch connector, PIO, power from the connector | **HW:** J2 and its power pins |
| Industrial boards and thin clients | +5 V on pin 20 (disk-on-module style) | **HW:** JP1 + F1 (jumper off by default) |
| Any host with a second drive | Master or slave, cable select, DASP#/PDIAG# handshake after reset, tri-stated when not selected | **HW:** jumpers, CSEL, open-drain DASP#/PDIAG#, buffer enables. **FW:** the reset handshake timing (device 1 asserts DASP# within 400 ms, PDIAG# within 31 s; check against ATA-4) |
| Hosts that poll a slow drive | Correct BSY/DRQ behaviour during SD stalls | **FW** (already designed: BSY covers every SD access) |

**Out of scope:**
- **Ultra DMA**, which would need new timing, CRC and cable detection.
- **ATAPI** (CD-ROM emulation): it's another command set, but it would need no new hardware.
- **SCSI and MFM/RLL hosts**, which use different buses.

**IOCS16# timing.** ISA hosts sample IOCS16# soon after the address is valid. The PIO program sees the chip select within about 50 ns (as measured in simulation for the prototype). Check that against ATA-2's IOCS16# timing. If it's too slow, decode it in logic from the buffered CS0#/DA lines instead: assert when CS0# = 0 and DA2–0 = 000. That frees the pin, at the cost of a few gates.

## 5. Things the prototype taught that the board must fix

| Prototype issue | Board fix |
|---|---|
| Unpowered RP2350 fed through its pins by the adapter | `Ioff` buffers on every bus line; the power-order rule disappears |
| IORDY bought time on every word, capping speed and failing hosts without IORDY | Data pre-loaded by DMA, and register reads answered from a table; IORDY kept only for rare slow paths |
| SD in SPI mode | SDIO 4-bit, pull-ups on all data lines, CLK termination, card detect |
| Hand-wired ringing on the strobes | Schmitt input buffers, short traces, ground plane, optional series resistors |
| Device 0 only, alone on the cable | Jumpers, CSEL, DASP#/PDIAG#, all outputs tri-stated when not selected |
| MK5: TPS63020 PG and the card detect not connected | Both on the expander |
| MK5: thin bulk capacitance on VSYS | Bulk capacitance sized per the supercap and amp datasheets |

## 6. Optional: the HDD Synth parts (merged board)

Carried over from the MK5 board (see the [HDDSynth_Native](https://github.com/MaverickUK/HDDSynth_Native) CLAUDE.md for its known issues):

| Qty | Part | Candidate MPN | Notes |
|---|---|---|---|
| 1 | I2S amplifier, mono, 3 W | MAX98357A | As MK5. SD_MODE on the expander |
| 1 | Speaker connector | JST PH 2-pin | — |
| 1 | OLED header | 1×4 2.54 mm, or JST SH 4-pin (Qwiic) | SSD1306 on the shared I2C bus |
| 1 | Rotary encoder with push | Alps EC11 series | On the expander |
| 1 | Action button | 6 mm tact | On the expander |
| 1 | RGB LED + I2C PWM driver | TI/NXP PCA9633 (4-channel) + a common-anode RGB LED | Replaces MK5's directly driven LED; no GPIO needed |
| 1 | Supercap backup boost | TI TPS61094 | As MK5. Also lets the PSRAM write cache be flushed on power loss. Revisit: the MK5 supercap failed after one deep discharge. Consider balancing, or a cut-off before full drain |
| 1 | Supercap | 1 F, 5.5 V | Prefer a part with a published ESR; check cell balancing |

## Open questions before layout

1. **Termination values.** Confirm against the ATA/ATAPI-4 or ATA/ATAPI-5 device termination table.
2. **Multiword DMA timings.** Firmware feasibility of MW DMA mode 2 at 150 MHz versus 200+ MHz. Prove it in `host/hddproto/pio_sim.py` first, as was done for PIO mode 0.
3. **The IORDY-less register path.** Prove the PIO table lookup in the simulator, with a host model that never waits (the ATA-1 case).
4. **IOCS16# from PIO or from logic** (section 4).
5. **Form factor.** One board with footprints for both J1 and J2, or two variants (3.5-inch and 2.5-inch).
6. **The cache and the supercap together.** A write-back cache without a guaranteed flush on power loss can corrupt the image. Decide whether the merged board always has the supercap, or the cache is write-through on boards without it.
