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
// ide_bus leaves it (ide_bus::updateIntrq drives it). `iordy` false (`iordy =
// off`, an Amiga's Gayle): IORDY is never the PIO's, so D's side-set reaches
// no pin and IORDY stays an input on its pull-up.
void begin(bool iordy);
// The write side's PIO block, claimed by begin() (pio_plan's table_write).
bool running();
// Once bus_observer has started: core 1 reads Status reads from its ring.
void attachObserver();

// ---- Core 1 ------------------------------------------------------------------

// The register image: what each read address answers, as R0-R2's entries
// hold it (bits 0-7 the value, 16-31 which DD lines to drive); publish adds
// each entry's address (bits 8-15).
// An entry's bits 16-31, which DD lines to drive: DD0-7 only. The registers are
// 8 bits, and a drive leaves DD8-15 to the host's pulls.
constexpr uint32_t DRIVE = 0x00FF0000;
struct Image {
    uint32_t error = 0, sector_count = 0, lba_low = 0, lba_mid = 0, lba_high = 0, device = 0, status = 0;
};
// Into the entries, Status (and Alternate Status) last, and as BSY while a
// busy write the DMA made isn't yet accounted for (the busy rule).
void publish(const Image& image);
// The entries but Status and Alternate Status: register writes taken mid-pass,
// whose read-backs can't wait for the pass to end (docs/amiga.md).
void publishPlain(const Image& image);
// One plain register's entry, as publish() writes it, straight after its write
// is taken: `address` the command block's (2 Sector Count to 6 Device). One
// store, for a read-back within a microsecond (docs/amiga.md).
void publishRegister(unsigned address, uint8_t value);

// The next write from W's ring (the sample as ide_bus's write samples read).
bool popWrite(uint32_t& sample);
// The register table's hot loop (main.cpp): up to `max` writes from W's ring
// into `out`, in order, each plain register's entry (Sector Count to Device)
// stored straight from its sample first, before the device has even seen it:
// a host can read one back a few hundred ns after writing it (docs/amiga.md).
// Not after a Command write in the batch (the device is busy from it). Only
// while the device is neither busy nor in a data phase (the caller's rule).
unsigned popWritesHot(uint32_t* out, unsigned max);
// bus_observer's ring, in order: its absolute position now, and whether a
// Status read (which acknowledges the interrupt) came at or after `since`.
// `each_read`, if given (the raw trace, console `t`), gets every read the
// ring holds as a bus sample (DIOR# low), as the other programs record them.
uint32_t observerPosition();
bool statusReadSince(uint32_t since, void (*each_read)(uint32_t sample) = nullptr);

// C's busy write: armed for the next Command write. Core 1 re-arms it once
// it has taken that command from W's ring.
// Console V: off the bus until restart().
void stop();
void restart();
void armCommandBusy();
// INTRQ (R1 lowers it as Status is answered): set when the device's interrupt changes.
uint32_t intrqRises();  // times core 1 drove INTRQ high (tableIntrq calls setIntrq only on a change)
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

}  // namespace ide_table
