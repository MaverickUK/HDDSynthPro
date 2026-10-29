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

#include "identify.h"

namespace bus_observer {

// Starts the program and its DMA channel. Call on core 1 next to
// iordy_timer::begin, so the two never race to claim a PIO. `identity` is
// the drive's, so IDENTIFY blocks on the bus can be checked too.
void begin(uint32_t disk_sectors, const IdentifyInfo& identity);

// Checks the samples so far; call often from core 0: data sectors with
// `check` (the disk is the generated one), and IDENTIFY blocks with `check`
// or `check_identify`. With neither the samples are discarded, which is
// cheap; checking every word costs core 0 enough to slow reads (1.23 to
// 1.16 MB/s), so on an image IDENTIFY is checked only around detection.
// Prints a BUSERR line for each block that was wrong on the bus.
void poll(Print& out, bool check, bool check_identify);

uint32_t sectorsChecked();
uint32_t sectorsBad();
uint32_t sectorsSkipped();
uint32_t identifyChecked();
uint32_t identifyBad();
uint32_t gaps();
uint64_t dmaWords();  // multiword DMA read words seen (checked like PIO data)
// The most samples ever waiting in the ring for core 0, of ringSize(): near
// the size, samples were overwritten before they were checked (gaps).
uint32_t ringMax();
uint32_t ringSize();
void resetCounts();

}  // namespace bus_observer
