// Slice 1 onwards: the ide_bus PIO program plus pin setup, and the core 1
// service step that connects it to the ATA device. Core 1 only.
//
// Not used by the slice 0 firmware yet; it is built so the SDK calls are
// checked, and its timing is proven in host/tests/test_pio_bus_sim.py.
#pragma once

#include <cstddef>
#include <cstdint>

#include "drive_device.h"
class FetchMailbox;

namespace ide_bus {

// Sets up the bus pins (DD and IORDY as PIO outputs that start released,
// INTRQ released, everything else an input) and starts the PIO program.
// `isa` (HDDSYNTH.INI): also start iordy_watchdog.pio, which releases IORDY
// when it has been held for about 23 us, for ISA cards (see that file).
// `early` (HDDSYNTH.INI `early_answers`): run ide_bus_early instead, which asks
// core 1 for each register's value at the chip select, for hosts that don't
// wait for IORDY (docs/no-iordy.md). No watchdog then: there's no room.
// `iordy` false (HDDSYNTH.INI `iordy = off`): IORDY stays an SIO input, so
// the programs' side-set on it does nothing, for hosts that check IORDY
// before starting a strobe (an Amiga 1200).
void begin(bool isa, bool early, bool iordy = true);

// Serves at most one register access. Call in a tight loop on core 1.
// Returns true if there was one; `sample` gets the pin sample for the trace.
bool serviceOnce(DriveDevice& dev, uint32_t& sample);

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
bool serveDataIn(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch);
// The same for ide_bus_early (`early_answers`), whose samples are requests and
// read reports.
bool serveDataInEarly(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch);
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
void serveBusEarly(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch,
                   uint32_t quiet_cycles, void (*timers)(DriveDevice&), void (*command)(DriveDevice&),
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
// released when the device doesn't drive it (ata::Device::intrqDriven). With
// the data lane, an interrupt waits while the device has moved on ahead of
// the host (docs/data-lane.md).
void updateIntrq(DriveDevice& dev);
// The data lane (docs/data-lane.md) serves data register reads: the default
// bus, unless `early_answers` or `isa` is on. Core 1's fast lanes, the raw
// trace's data reads and setDataReadDelay() don't apply then.
bool dataLaneOn();
// Which program serves the bus, for `s`: "lane" (ide_bus_data with ide_data_in,
// the default), "ide_bus" (`isa = on`), "early" or "noiordy" (`early_answers`).
const char* busName();
// updateIntrq(), after first keeping the data lane in step with the device:
// the device's next data-in block onto the lane before INTRQ can announce it.
// Core 1 calls it where a block can begin: after running a command, and when
// a fetch completes (before every register read is answered, ide_bus does it
// itself). True while a block is on the lane. Without the lane, updateIntrq().
bool syncDataLane(DriveDevice& dev);
// A block is on the lane (core 1's loop then calls serveLane).
extern bool lane_busy;
// While a block is on the lane (lane_busy): waits for its end and gets the
// next ready, posting the read-ahead and taking its result meanwhile. True
// when an access is waiting or the blocks are done (back to the bus); false
// when a command is queued or RESET# leaves `reset_low` (carry on with the
// pass, which runs the command or handles the reset).
bool serveLane(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch);
// The lane's blocks started, and the longest core 1 took to move it from one
// block to the next (a status read arriving meanwhile waits): `s`'s
// lane_blocks and lane_switch_max_ns. resetLaneStats() clears the longest (`r`).
uint32_t laneSwitches();
uint32_t laneSwitchMaxNs();
void resetLaneStats();

// early_answers: lets IORDY go if the drive has held it cfg::IORDY_STALL_US
// since a chip select with no strobe (the tight loops call it themselves;
// core 1's main loop calls it on each pass). iordyStallReleases() counts them.
void checkIordyStall();
uint32_t iordyStallReleases();
// Interrupts raised since start-up (INTRQ driven high from low).
uint32_t intrqRises();

// Diagnostic (ide_bus.cpp's EARLY_PROFILE, off): early answers that came
// after the host's strobe had started, by what core 1 was doing, for `s`
// (" early_requests=... early_late=site:late/over_1us/max_ns,..."). Returns
// the length written, 0 when not built in. resetEarlyProfile() clears it (`r`).
size_t formatEarlyProfile(char* buf, size_t size);
void resetEarlyProfile();
// Debug (console `D`): answer every data register read at least `ns` later,
// to find how long the host really waits on IORDY. 0 turns it off.
void setDataReadDelay(uint32_t ns);

}  // namespace ide_bus
