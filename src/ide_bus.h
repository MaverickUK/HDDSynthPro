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
// `isa` (HDDSYNTH.INI): also start iordy_watchdog.pio, which releases IORDY
// when it has been held for about 23 us, for ISA cards (see that file).
// `early` (HDDSYNTH.INI `early_answers`): run ide_bus_early instead, which asks
// core 1 for each register's value at the chip select, for hosts that don't
// wait for IORDY (docs/no-iordy.md). No watchdog then: there's no room.
void begin(bool isa, bool early);

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
// The same for ide_bus_early (`early_answers`), whose samples are requests and
// read reports.
bool serveDataInEarly(ata::Device& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch);
// early_answers' tight bus loop, after an access: serves accesses inline and
// keeps fetches moving until the bus is quiet for `quiet_cycles` (core 1's
// cycle counter), a command or data block needs the main loop, or RESET#
// leaves `reset_low`.
// `timers` runs the device's BSY timers (the data-out hold, waking), which
// would otherwise wait for the main loop while the host polls status.
// `command` runs a queued command (and records it), so the loop needn't
// return for one.
// `bus`, if not null, records each access served (the raw trace), not the
// answer requests.
void serveBusEarly(ata::Device& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch,
                   uint32_t quiet_cycles, void (*timers)(ata::Device&), void (*command)(ata::Device&),
                   void (*bus)(uint32_t) = nullptr);

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

// Drives INTRQ to match the device: high when asserted, low when not, and
// released when the device doesn't drive it (ata::Device::intrqDriven).
void updateIntrq(const ata::Device& dev);
// Interrupts raised since start-up (INTRQ driven high from low).
uint32_t intrqRises();

// Debug (console `D`): answer every data register read at least `ns` later,
// to find how long the host really waits on IORDY. 0 turns it off.
void setDataReadDelay(uint32_t ns);

}  // namespace ide_bus
