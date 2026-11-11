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

// Stops both programs and frees their state machines and instructions, so
// another user can have the PIO block (the PCB's card borrowing it for SDIO);
// begin() starts them again. The counts are kept. Core 0 only.
void end();
bool running();

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

// Reads the host ended while IORDY was still low, so it didn't wait for the
// data (iordy_check.pio): should be 0. ATA lets hosts ignore IORDY in PIO
// modes 0-2, and some older controllers do; they'd read garbage. The last
// such read's register.
uint32_t ignoredReads();
const char* ignoredRegister();
// Of those, the data register's (a PCI controller may time its data port
// apart from the other registers, and ignore IORDY there: the Evesham's CD
// reads, docs/atapi.md), and when core 0 collected the last one's report
// (time_us_32(), within a loop pass of the read).
uint32_t ignoredDataReads();
uint32_t ignoredAtUs();

// The data register's reads held past the threshold, by how long: up to
// 700 ns, 1,000, 1,250 (ATA's limit), and longer; and the longest. How late
// core 1 answers data words, apart from the status polls that dominate the
// figures above.
constexpr int DATA_BUCKETS = 4;
constexpr uint32_t DATA_BUCKET_EDGES_NS[DATA_BUCKETS - 1] = {700, 1000, 1250};
uint32_t dataSlowReads(int bucket);
uint32_t dataMaxNs();
// The reads held past ATA's limit (cfg::IORDY_LIMIT_NS), one at a time, for
// a SLOWREAD line each: how long, which register, and when core 0 collected
// the report. The last few are kept; older ones are dropped if nobody takes them.
struct OverLimit {
    uint32_t ns;
    const char* reg;
    uint32_t at_us;
};
bool takeOverLimit(OverLimit& read);  // ns 0: a data read that started with IORDY high (EARLYDATA)
uint32_t earlyDataReads();
uint32_t contestedAccesses();  // fast peripheral bus, since the last reset
uint32_t stallCycles();
void reset();

// Asks for a reset from either core; the next poll() does it.
void requestReset();

}  // namespace iordy_timer
