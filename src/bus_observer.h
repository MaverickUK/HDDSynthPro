// Drive build: the bus_observer PIO program, which samples the bus at the
// end of every read, and the check of those samples against the generated
// disk (sector_check). It shows whether data the host got wrong was wrong on
// the cable, or right on the cable and spoiled after it.
//
// DMA copies the samples into a ring in RAM, so core 0 can fall behind for a
// while (about 2 ms of reads) without losing any.
#pragma once

#include <Arduino.h>

#include <cstdint>

namespace bus_observer {

// Starts the program and its DMA channel. Call on core 1 next to
// iordy_timer::begin, so the two never race to claim a PIO.
void begin(uint32_t disk_sectors);

// Checks the samples so far; call often from core 0. With `check` false
// (the disk isn't the generated one) they are only discarded. Prints a
// BUSERR line for each sector that was wrong on the bus.
void poll(Print& out, bool check);

uint32_t sectorsChecked();
uint32_t sectorsBad();
uint32_t sectorsSkipped();
uint32_t gaps();
void resetCounts();

}  // namespace bus_observer
