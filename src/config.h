// Pin map and sizes. Pure constants, so host tests can include it.
// docs/wiring.md maps each pin to the IDE header.
#pragma once

#include <cstddef>

namespace cfg {

// The bus uses GPIO 4-30, all inside one PIO block's GPIO 0-31 window.
// Avoided on purpose:
//   GPIO 0-3  the BB48 pulls GPIO 2/3 up to 3.3 V through 2.2k (R17/R18,
//             UEXT/Qwiic I2C); GPIO 0/1 stay free for a UART if needed.
//   GPIO 25   the on-board LED (2.2k + LED to GND).
// DD, DA and CS# are on consecutive pins so one PIO read captures an access.
constexpr unsigned PIN_DD0 = 4;      // DD0-DD15 on GPIO 4-19
constexpr unsigned PIN_DA0 = 20;     // DA0-DA2 on GPIO 20-22
constexpr unsigned PIN_CS0_N = 23;   // CS0# (ATA "CS1FX-"): command block registers
constexpr unsigned PIN_CS1_N = 24;   // CS1# (ATA "CS3FX-"): control block registers
constexpr unsigned PIN_BOARD_LED = 25;  // on-board LED, not a bus line
constexpr unsigned PIN_DIOR_N = 26;
constexpr unsigned PIN_DIOW_N = 27;
constexpr unsigned PIN_IORDY = 28;   // host pulls up to 5 V via 1k: only ever drive low or release
constexpr unsigned PIN_INTRQ = 29;
constexpr unsigned PIN_RESET_N = 30;

// Multiword DMA's handshake (docs/dma.md). Wired on the PCB, and on the BB48
// rig since 29 Oct 2026 (IDE pin 21 to GPIO 31, pin 29 to GPIO 3). DMARQ is
// driven only during a DMA command; DMACK# isn't read yet (Ultra DMA will).
constexpr unsigned PIN_DMARQ = 31;
constexpr unsigned PIN_DMACK_N = 3;

// IOCS16# (IDE pin 32), for 16-bit ISA cards that pass it to the ISA bus
// (iocs16.pio). Open drain: pulled low while the data register is addressed,
// otherwise released. GPIO 2 is the only free pin in the PIO window (as PCB
// rev 0.4 plans); the BB48's 2.2k pull-up on it (R17) is harmless for an open
// drain signal the host also pulls up. Wired on the rig since 30 Oct 2026.
constexpr unsigned PIN_IOCS16_N = 2;

// early_answers: after each access, core 1 watches only the bus this long, so
// it answers the next request within the 160 ns a host that doesn't wait for
// IORDY leaves (test_pio_bus_early_sim.py), not after a bookkeeping step.
constexpr uint32_t EARLY_BUS_ONLY_US = 50;

// PCB only (rev 0.3; on the BB48 rig these GPIOs are unconnected, so driving
// and reading them there is harmless). The IDE bus goes through bus switches
// that start off: BUS_EN# held high by a pull-up until the firmware drives it
// low (closing JP1 bypasses it). SD_CD is the card socket's detect switch,
// pulled up, low with a card in (to be checked on the first board).
constexpr unsigned PIN_SD_CD = 40;
constexpr unsigned PIN_BUS_EN_N = 41;

// Whether `mode = auto` offers multiword DMA where DMARQ is wired is the
// `dma` setting in HDDSYNTH.INI (drive_config::DEFAULT_DMA: off until DMA has
// had a soak). Never offered without DMARQ, whatever it says.

// The activity link to an HDD Synth (docs/activity-link.md): UART0 TX on
// GPIO 0 ($HDA, $HDE); UART0 RX on GPIO 1, pulled up, for the synth's $HDM
// (protocol version 2, seek_timing = synth).
constexpr unsigned PIN_LINK_TX = 0;
constexpr unsigned PIN_LINK_RX = 1;
constexpr uint32_t LINK_BAUD = 115200;
constexpr uint32_t LINK_HEARTBEAT_MS = 200;  // a sentence at least this often
constexpr uint32_t LINK_HEAD_GAP_US = 2000;  // $HDE at most this often (about its time on the wire)

constexpr unsigned BUS_FIRST_PIN = PIN_DD0;
constexpr unsigned BUS_LAST_PIN = PIN_RESET_N;

// The microSD breakout (slice 5). The same wires serve SPI now and 4-bit
// SDIO later: CLK, CMD and DAT0 are on SPI0's SCK, TX and RX pins, DAT3 is
// the SPI chip select, and DAT0-3 are consecutive, as PIO SDIO needs.
// CMD and DAT0-3 have 10k pull-ups to 3.3 V on the breakout's side.
constexpr unsigned PIN_SD_CLK = 34;   // SPI0 SCK
constexpr unsigned PIN_SD_CMD = 35;   // SPI0 TX (card DI)
constexpr unsigned PIN_SD_DAT0 = 36;  // SPI0 RX (card DO); DAT1-3 on 37-39
constexpr unsigned PIN_SD_DAT3 = 39;  // SPI chip select
// The SPI clock for the card, MHz. SPI0 divides the peripheral clock, which
// main.cpp keeps at the 200 MHz system clock, by an even number, so this
// rounds down: 25 is exact (÷8), the next step is 33.3 (÷6). At 150 MHz (up
// to 0.13.0) 25 was ÷6, and 37.5 didn't start on the hand-wired leads. Each
// step up was checked by hashing the whole image.
constexpr uint32_t SD_SPI_MHZ = 25;

// After a command starts, core 1 only serves the bus for this long before its
// next bookkeeping step, so the host's first status poll (the adapter's comes
// at once after IDENTIFY and SET FEATURES) never waits behind one. Without it
// that poll was held 1.29-1.39 us (ATA allows 1.25).
constexpr uint32_t COMMAND_QUIET_US = 4;

// Trace ring between core 1 (producer) and core 0 (consumer):
// 8192 events x 12 bytes = 96 KB.
constexpr size_t TRACE_RING_EVENTS = 8192;

// The listen build's capture window (console `w`): with no drive answering,
// the adapter probes about once a microsecond forever, faster than core 0 can
// print, so the ring overflows. With the window on, each RESET# assert opens
// a window of this many bus accesses, and the rest are counted but not
// recorded until the next one. Two windows fit in the ring, so the adapter's
// two detection resets never drop an event.
constexpr uint32_t LISTEN_WINDOW_EVENTS = 3000;
static_assert(2 * LISTEN_WINDOW_EVENTS + 64 < TRACE_RING_EVENTS, "two windows must fit in the trace ring");

// Core 0 prints a run of identical accesses once it has been quiet this long.
constexpr unsigned TRACE_FLUSH_MS = 20;

// How long IDLE IMMEDIATE shows BSY when it wakes the drive from standby. A CF
// card on the rig took about 25 us; a spinning disk takes seconds.
constexpr uint32_t WAKE_BUSY_US = 50;

// After each data-out block but the last, BSY for at least this long before
// DRQ and INTRQ ask for the next one. The NS1081 reads Status straight after
// a block, clearing any interrupt, and then waits for INTRQ: with write-behind
// the next block could be wanted at once, and it would wait forever.
constexpr uint32_t DATA_OUT_BSY_US = 10;

// The BB48's LED is the drive's activity light: lit while a command runs, and
// for at least this long, so a command of a few microseconds still shows.
constexpr uint32_t LED_MIN_MS = 25;

// The RAM overlay on the generated disk (block_source.h): how many distinct
// sectors can be written, at 512 bytes each.
constexpr size_t RAM_DISK_SECTORS = 512;

// The IORDY timer (iordy_timer.pio) counts and times reads that IORDY holds
// longer than this. ATA's limit is 1250 ns (tB).
constexpr uint32_t IORDY_REPORT_NS = 400;
// Reads held past ATA's limit also print a SLOWREAD line each.
constexpr uint32_t IORDY_LIMIT_NS = 1250;

}  // namespace cfg
