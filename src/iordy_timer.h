// Drive build: the iordy_timer PIO program, which times reads from the host's
// side (how long IORDY holds each one) and reports the slow ones. Core 0
// drains the reports; console `s` shows them and `r` clears them.
//
// Alongside, two bus fabric performance counters count contested accesses
// and stall cycles on the fast peripheral bus (where the PIO is), to show
// whether the other core, USB or DMA is delaying core 1's PIO accesses.
#pragma once

#include <cstdint>

namespace iordy_timer {

// Starts the program. Reads that IORDY holds longer than `threshold_ns`
// are counted and timed.
void begin(uint32_t threshold_ns);

// Collects the reports so far. Call often from core 0.
void poll();

// Reads held longer than the threshold since the last reset, and the longest:
// how long, which register it read (e.g. "STATUS"), and when core 0 collected
// its report (time_us_32(), shortly after the read).
uint32_t slowReads();
uint32_t maxNs();
const char* maxRegister();
uint32_t maxAtUs();
// Reads that started while IORDY was still high, so the host may not have
// waited for the data, and the register the last one read. Should stay 0.
uint32_t earlyReads();
const char* earlyRegister();
// The reads held past ATA's limit (cfg::IORDY_LIMIT_NS), one at a time, for
// a SLOWREAD line each: how long, which register, and when core 0 collected
// the report. The last few are kept; older ones are dropped if nobody takes them.
struct OverLimit {
    uint32_t ns;
    const char* reg;
    uint32_t at_us;
};
bool takeOverLimit(OverLimit& read);
uint32_t contestedAccesses();  // fast peripheral bus, since the last reset
uint32_t stallCycles();
void reset();

// Asks for a reset from either core; the next poll() does it.
void requestReset();

}  // namespace iordy_timer
