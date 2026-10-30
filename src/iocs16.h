// Drive build: the iocs16 PIO program, which pulls IOCS16# (IDE pin 32) low
// while the host addresses the data register, so a 16-bit ISA card that
// passes it to the ISA bus does 16-bit data cycles (iocs16.pio).
//
// It needs a PIO block of its own, being too big for the diagnostics' spare
// room: it takes ide_dma's, so it runs only when the drive doesn't offer
// multiword DMA (an ISA-era host never uses it).
#pragma once

namespace iocs16 {

// Starts the program. Call after ide_bus and the diagnostics have their PIO
// blocks, and instead of ide_dma::begin().
void begin();

}  // namespace iocs16
