// The register table (docs/register-table.md): a bus program that answers
// every read from values core 1 keeps in the PIO's FIFO registers, so no read
// waits for core 1. ide_table.pio (the read side, owning DD and IORDY) and
// ide_table_write.pio (the write side, beside bus_observer), with the DMA
// channels between them. Console `P t`. The hard disk only, without DMA.
#pragma once

#include <cstdint>

namespace ide_table {

// The bus pins, the read side's four state machines, the write side's three
// and their DMA channels. In place of ide_bus::begin(); INTRQ is set up as
// ide_bus leaves it (ide_bus::updateIntrq drives it).
void begin();
// The write side's PIO block, claimed by begin() (pio_plan's table_write).
bool running();
// Once bus_observer has started: core 1 reads Status reads from its ring.
void attachObserver();

// ---- Core 1 ------------------------------------------------------------------

// The register image: what each read address answers, as R0-R2's entries
// hold it (bits 0-7 the value, 16-31 which DD lines to drive); publish adds
// each entry's address (bits 8-15).
struct Image {
    uint32_t error = 0, sector_count = 0, lba_low = 0, lba_mid = 0, lba_high = 0, device = 0, status = 0;
};
// Into the entries, Status (and Alternate Status) last, and as BSY while a
// busy write the DMA made isn't yet accounted for (the busy rule).
void publish(const Image& image);

// The next write from W's ring (the sample as ide_bus's write samples read).
bool popWrite(uint32_t& sample);
// bus_observer's ring, in order: its absolute position now, and whether a
// Status read (which acknowledges the interrupt) came at or after `since`.
uint32_t observerPosition();
uint32_t observerRingIndex();  // bench
void snapObserver(uint32_t* out, unsigned n, uint32_t& end);  // bench
bool statusReadSince(uint32_t since);

// C's busy write: armed for the next Command write. Core 1 re-arms it once
// it has taken that command from W's ring.
// Console V: off the bus until restart().
void stop();
void restart();
void armCommandBusy();
bool commandBusyPending();
// INTRQ (R1 lowers it as Status is answered): set when the device's interrupt changes.
void setIntrq(bool asserted, bool driven);  // fired, and the command not yet taken

// A data-in block: the DMA stream into D's FIFO under way (its completion
// starts the busy write). Before publishing DRQ.
void startDataIn(const uint16_t* words, uint16_t count);
bool dataInActive();
bool dataInDone();   // the host has read the last word
void stopDataIn();   // done, or cut short

// A data-out block: WD's channel writes `count` words to `dest`, then the
// busy write. Before publishing DRQ.
void startDataOut(uint16_t* dest, uint16_t count);
bool dataOutActive();
bool dataOutDone();
void stopDataOut();

// For `s`: busy writes made, and blocks.
uint32_t blocksIn();
uint32_t blocksOut();
void debug(uint32_t out[6]);  // bench
uint32_t selfTest(uint16_t word);
bool selfTestReleased();
uint32_t readD(int reg);
uint32_t* streamTest();  // bench: 12 words
void blockRecords(uint32_t out[81]);  // bench
void stopWhy(uint8_t why);  // bench: 1 done, 2 dropped, 3 command, 4 reset
uint32_t* fakeReads();  // bench: 4 words
uint32_t fakeInfo(unsigned i);
void fakeCpu(bool on);
void fakeZero(bool on);
bool benchHold();
void notePass(uint32_t extra);
void armCommandCapture();  // bench: capture from the next Command write
const volatile uint32_t (*seenLog(uint32_t& n))[3];
uint32_t capture(const uint32_t*& pins, const uint16_t*& d);
void fakeCycles(uint32_t c);
uint32_t busyTest(uint32_t wait_cycles);  // bench  // bench: cycles low and high per faked read  // bench: core 1 leaves the blocks alone
uint32_t flushTest();  // bench: DD0's padout, its funcsel, the PIO number  // bench: D's Y, X or ISR (pio_src_dest), on DD  // bench: `mov pindirs, null` released them after  // bench: D drives `word`; the lines seen, and (top half) their output enables

}  // namespace ide_table
