// Drive build: the iordy_timer PIO program, which times reads from the host's
// side (how long IORDY holds each one) and reports the slow ones. Core 0
// drains the reports; console `s` shows them and `r` clears them.
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
void reset();

// Asks for a reset from either core; the next poll() does it.
void requestReset();

}  // namespace iordy_timer
