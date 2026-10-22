// Slice 0 bus listener: the ide_listen PIO program plus pin setup.
// Every bus pin is an input; nothing is ever driven. Core 1 only.
#pragma once

#include <cstdint>

namespace listen {

// Sets the bus pins up as plain inputs (no pulls) and starts the PIO program.
void begin();

// Takes one captured access (a pin sample, see bus_sample.h) if there is one.
bool pop(uint32_t& sample);

// True once after the PIO stalled on a full FIFO (accesses were missed).
bool takeStall();

// Current level of GPIO 0-31.
uint32_t pinsNow();

}  // namespace listen
