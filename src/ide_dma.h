// Multiword DMA's data phase on the bus (docs/dma.md): ide_dma.pio in a PIO
// block of its own, fed or drained by an RP2350 DMA channel. ide_bus keeps
// every register access. Core 1 only, except begin().
//
// DMARQ (cfg::PIN_DMARQ) is driven only while a DMA command runs: otherwise
// its output is forced off and the host's pull-down holds it low. For a read
// burst DD0-15 are handed to this block (a GPIO is driven by one PIO block
// at a time), and handed back to ide_bus's when the burst ends, so a status
// read between bursts is answered as usual.
#pragma once

#include <cstdint>

namespace ata { class Device; struct DmaBurst; }

namespace ide_dma {

// Claims a free PIO block for both programs, a state machine and a DMA
// channel, and leaves DMARQ undriven. Call after ide_bus::begin().
void begin();

// Core 1: moves `burst` (a sector) between the host and the device's buffer.
// DMARQ goes high, the host strobes the words, DMARQ falls on the last one.
void startBurst(const ata::DmaBurst& burst);

// Core 1: true once the burst's last word is moved (then DD is back with
// ide_bus). False while it runs, or if none was started.
bool burstDone();
bool burstRunning();

// Core 1: the DMA command is over, finished or cut short (RESET#, SRST, a new
// command): stops any burst, releases DD and DMARQ, and readies the program
// for the next command. Cheap when nothing ran.
void endCommand();

// True from startBurst() until endCommand().
bool active();

}  // namespace ide_dma
