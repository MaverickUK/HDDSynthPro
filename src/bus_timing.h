// Drive build: bus_timing.pio, which measures each read's t1 (chip select to
// DIOR# falling) and strobe (DIOR# low), to size the no-IORDY design
// (docs/no-iordy.md) to a host that doesn't wait. Core 0 drains it.
#pragma once

#include <cstdint>

namespace bus_timing {

// Starts the program in a PIO block with room; false if there's none (with
// multiword DMA offered, ide_dma fills the third block).
bool begin();
bool running();

// Collects the reports so far. Call often from core 0.
void poll();

// Prints, as `RDTIME t1_ns=.. strobe_ns=..` lines, up to `max` reports not yet
// printed out of the first PRINT_LIMIT since the last reset(). Core 0, only
// when USB serial has room.
void printPending(unsigned max);

// Timed reads since reset(), and the shortest and longest of each.
uint32_t count();
uint32_t t1MinNs();
uint32_t t1MaxNs();
uint32_t strobeMinNs();
uint32_t strobeMaxNs();

// Forgets the counts, and prints the next PRINT_LIMIT reports again (console `r`).
void reset();

constexpr unsigned PRINT_LIMIT = 64;

}  // namespace bus_timing
