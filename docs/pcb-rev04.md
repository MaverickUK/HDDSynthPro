# PCB rev 0.4: what to add

Rev 0.3 (the `hardware/` KiCad project, sent for manufacture on 28 Oct 2026) is the minimum that runs today's firmware: the IDE bus behind four SN74CB3T3245 bus switches, DMARQ and DMACK#, microSD wired for SDIO with card detect, the activity link, USB-C and the 5 V inputs. [pcb-bom.md](pcb-bom.md) holds the fuller design targets and their reasons; this page lists what rev 0.4 should add so the rest is a firmware change later, in priority order, with what the rig has taught since.

GPIO numbers are suggestions from rev 0.3's free pins: **2** (the only free pin in the bus program's GPIO 0-31 window), **32, 33, 42-47**. Whatever is chosen goes into `src/config.h` first, and `hardware/tools/check_netlist.py` checks the board against it.

## 1. The IDE signals rev 0.3 leaves unconnected (must)

Rev 0.3 has four spare bus-switch channels (U6 A1-A4/B1-B4): exactly enough.

| IDE pin | Signal | Unlocks | GPIO | Notes |
|---|---|---|---|---|
| 39 | DASP# | Master/slave with a real drive on the same cable; the PC case's disk light | 32 | Open drain. At power-up a slave asserts it to say it's there |
| 34 | PDIAG# / CBLID | The master/slave reset handshake; Ultra DMA 3+ detects an 80-wire cable on it | 33 | Open drain. The slave reports its diagnostics to the master on it |
| 28 | CSEL | Cable select: master or slave by the cable's connector | 42 | Read at reset |
| 32 | IOCS16# | ISA-era hosts (286, 386, many 486 ISA cards) that use it to choose 16-bit data cycles | **2** | Must answer within tens of ns of the chip select, as IORDY does, so from the bus program's PIO, inside its window. Or decode it in logic (CS0# low and DA2-0 = 000), freeing GPIO 2 ([pcb-bom.md](pcb-bom.md), section 4) |

Without DASP# and PDIAG#, sharing a cable is unsafe, not just unsupported: a real master that doesn't see a slave may answer the absent slave's register reads, and so would fight our drive on the bus (and the reverse). Today the rule is one drive per cable, ours as master.

## 2. PSRAM (strongly recommended)

An 8 MB QSPI PSRAM on the RP2350's second memory chip select, **GPIO 47** (its options are 0, 8, 19 and 47; only 47 is free). About a dollar. It unlocks a read cache (the card is now the limit: about 1.8 MB/s over SPI, where multiword DMA alone reaches 5 MB/s from RAM), write-back caching, and a RAM disk far larger than today's 256 KB. A write-back cache needs a decision about power loss ([pcb-bom.md](pcb-bom.md), open question 6).

## 3. The microSD card

- **Prove SDIO before committing the layout.** On the rig (29 Oct 2026) SDIO was unreliable on the hand-wired leads: at 25 MHz directory lookups failed, at 12.5 MHz a write dropped the card ([pio-allocation.md](pio-allocation.md)). Rev 0.3's short traces should fix that; test SDIO on the first rev 0.3 boards before rev 0.4's layout is frozen, and keep the 10 kΩ pull-ups on CMD and DAT0-3.
- **A card power switch:** a small P-FET or load switch on the card's 3.3 V, from a spare GPIO (43). A card left in SPI mode stays there until power is cut, and a card that locks up can only be reset that way. With it, the firmware can move the card from SPI to SDIO without a reinsertion and recover a stuck card.

## 4. Signal integrity for faster modes (cheap insurance)

**Footprints for series resistors on DD0-15** (0402, fitted with 0 Ω). Rev 0.3 has 33 Ω only on DIOR# and DIOW# (R14, R15). Ultra DMA above mode 2 is sensitive to reflections, and ATA recommends 22-33 Ω at the drive; with footprints they can be fitted if needed, and cost nothing if not.

## 5. A debug header (cheap insurance)

A 2.54 mm header with a few spare GPIOs (44-46, and 32/33 if they aren't used above) and ground. The open timing questions (write-behind's slow reads on the card) need core 1 to mark events on pins for a logic analyser, alongside DIOR#, IORDY and INTRQ; a header makes that a clip-on job.

## 6. Nice to have

- **Power from a retro PC:** a 4-pin Molex or floppy (Berg) input, diode-ORed like rev 0.3's other inputs, so the board can live inside the PC. (The 44-pin 2.5-inch connector's power pins are another route: [pcb-bom.md](pcb-bom.md), section 4.)
- **A user button or 2-way DIP switch:** master/slave or image choice without a console. `HDDSYNTH.INI` can do both, so this is convenience.

## Already right in rev 0.3 (keep)

- **DMARQ on GPIO 31 and DMACK# on GPIO 3**, inside the bus program's window. Multiword DMA works on the rig on those pins (0.16.2: reads 3.8 times PIO mode 3, writes 5.5 times). The firmware tests DMARQ at start-up with a pull-up against the host's pull-down (0.16.4), so on a board the bus switches must be on (BUS_EN#, GPIO 41) before that test.
- **The bus switches with Ioff**: they isolate the bus when the board is unpowered, so the BB48's power-order rule goes away.
- **SD card detect on GPIO 40**: the re-mount code (0.13.0) can use it instead of probing.
- **DAT0-3 on consecutive GPIOs (36-39)**, as PIO SDIO needs, and CLK/CMD on 34/35.

## Firmware for rev 0.3 itself

Done in 0.16.6: the firmware drives **BUS_EN# (GPIO 41) low** once the bus pins are set up, so JP1 can stay open, and `p` reads **SD_CD (GPIO 40)**. The first day with the board: [pcb-bringup.md](pcb-bringup.md).
