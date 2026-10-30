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
//
// Spot checks (the default): the drive comes first. `serve` (core 0's fetch
// handler) is called every few words, so a fetch never waits behind a batch
// of checks, and when the checks fall behind (multiword DMA outruns them:
// a check costs more of core 0 than serving the sector) the waiting samples
// are dropped on purpose, counted in dropped(), and checking resumes at the
// next sector start. Every sector of PIO and of short DMA commands is
// checked, a few percent of long DMA transfers. Full checks (setFull):
// checks come first, as before 0.18.1, and slow multiword DMA reads to
// about 2.2 MB/s. Either way, samples core 0 may have let DMA overwrite
// are dropped, counted in lost().
void poll(Print& out, bool check, bool check_identify, void (*serve)() = nullptr);

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
uint64_t dropped();  // samples dropped unchecked because the checks were behind
// Times the ring may have been overrun: core 0 away longer than DMA takes to
// fill it. The sequence numbers can't show that: they repeat every 2,048
// reads, and the ring holds 4,096.
uint32_t lost();
void setFull(bool full);
bool full();
void resetCounts();

}  // namespace bus_observer
