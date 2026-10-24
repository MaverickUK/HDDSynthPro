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

// Trace ring between core 1 (producer) and core 0 (consumer):
// 8192 events x 12 bytes = 96 KB.
constexpr size_t TRACE_RING_EVENTS = 8192;

// Core 0 prints a run of identical accesses once it has been quiet this long.
constexpr unsigned TRACE_FLUSH_MS = 20;

// How long IDLE IMMEDIATE shows BSY when it wakes the drive from standby. A CF
// card on the rig took about 25 us; a spinning disk takes seconds.
constexpr uint32_t WAKE_BUSY_US = 50;

// The BB48's LED is the drive's activity light: lit while a command runs, and
// for at least this long, so a command of a few microseconds still shows.
constexpr uint32_t LED_MIN_MS = 25;

// The RAM overlay on the generated disk (block_source.h): how many distinct
// sectors can be written, at 512 bytes each.
constexpr size_t RAM_DISK_SECTORS = 512;

// The IORDY timer (iordy_timer.pio) counts and times reads that IORDY holds
// longer than this. ATA's limit is 1250 ns (tB).
constexpr uint32_t IORDY_REPORT_NS = 400;

}  // namespace cfg
