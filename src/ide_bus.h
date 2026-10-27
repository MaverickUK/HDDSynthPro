// Slice 1 onwards: the ide_bus PIO program plus pin setup, and the core 1
// service step that connects it to the ATA device. Core 1 only.
//
// Not used by the slice 0 firmware yet; it is built so the SDK calls are
// checked, and its timing is proven in host/tests/test_pio_bus_sim.py.
#pragma once

#include <cstdint>

namespace ata { class Device; }
class FetchMailbox;

namespace ide_bus {

// Sets up the bus pins (DD and IORDY as PIO outputs that start released,
// INTRQ released, everything else an input) and starts the PIO program.
void begin();

// Serves at most one register access. Call in a tight loop on core 1.
// Returns true if there was one; `sample` gets the pin sample for the trace.
bool serviceOnce(ata::Device& dev, uint32_t& sample);

// The fast lane for a data-in block (the 256 words of a sector or IDENTIFY):
// answers data reads straight from the device's buffer until the block is
// read, an access that isn't a data read comes (served as serviceOnce would),
// or RESET# leaves the level `reset_low`. Returns false in that last case, so
// the caller handles the reset. Call only when dev.dataInRemaining() is true.
// It records nothing for the raw trace and ignores setDataReadDelay(), so the
// caller skips it for those. While it waits between words it does one small
// job per pass: post the device's read-ahead to core 0 (once, after the first
// data word; `posted_fetch` is the last id posted), or hand core 0's results
// to the device. So a read-ahead is under way while the host reads, it's in
// when the block ends, and the next sector's DRQ comes at once; and a read
// never waits behind more than one job, nor a status read behind any.
bool serveDataIn(ata::Device& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch);

// True while setDataReadDelay() is on.
bool dataReadDelayed();

// Starts the PIO program again with empty FIFOs and DD and IORDY released.
// Call when the host releases RESET#. While the host is unplugged or powering
// up its strobes float low, and the PIO can push a write-path sample with
// DIOR# low too. Core 1 answers that as a read, the PIO never pulls the
// answer, and every later read would get the previous read's data.
void restart();

// Stops the PIO program and releases DD, IORDY and INTRQ: the drive is gone
// from the bus, as if unplugged, until restart(). Console `V`.
void stop();

// Drives INTRQ to match the device: asserted = driven high, otherwise
// released (the host pulls it down).
void updateIntrq(bool asserted);

// Debug (console `D`): answer every data register read at least `ns` later,
// to find how long the host really waits on IORDY. 0 turns it off.
void setDataReadDelay(uint32_t ns);

}  // namespace ide_bus
