// Slice 1 onwards: the ide_bus PIO program plus pin setup, and the core 1
// service step that connects it to the ATA device. Core 1 only.
//
// Not used by the slice 0 firmware yet; it is built so the SDK calls are
// checked, and its timing is proven in host/tests/test_pio_bus_sim.py.
#pragma once

#include <cstdint>

namespace ata { class Device; }

namespace ide_bus {

// Sets up the bus pins (DD and IORDY as PIO outputs that start released,
// INTRQ released, everything else an input) and starts the PIO program.
void begin();

// Serves at most one register access. Call in a tight loop on core 1.
// Returns true if there was one; `sample` gets the pin sample for the trace.
bool serviceOnce(ata::Device& dev, uint32_t& sample);

// True if no access is waiting in the PIO's FIFO.
bool idle();

// Starts the PIO program again with empty FIFOs and DD and IORDY released.
// Call when the host releases RESET#. While the host is unplugged or powering
// up its strobes float low, and the PIO can push a write-path sample with
// DIOR# low too. Core 1 answers that as a read, the PIO never pulls the
// answer, and every later read would get the previous read's data.
void restart();

// Drives INTRQ to match the device: asserted = driven high, otherwise
// released (the host pulls it down).
void updateIntrq(bool asserted);

}  // namespace ide_bus
