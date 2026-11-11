// Which bus program the drive would choose for the host it's plugged into
// (docs/auto-bus.md): early answers, safe everywhere, or the data lane,
// faster but only for hosts that always wait for IORDY and aren't ISA cards.
//
// Step one of the design: this only advises. Core 0 feeds it the counters
// it already keeps and prints what it would have chosen, and when (AUTOBUS
// lines, `autobus` in `s`); nothing switches. Each PC run so then shows
// whether the rule would have been right before anything relies on it.
//
// The rule, since the latest reset (RESET# release, or boot):
//   - no DMA offered (with DMA the data doesn't go through the program);
//   - the evidence available: the bus observer's count of DIOR# with no chip
//     select, and iordy_check's count of reads the host didn't wait for;
//   - no ISA signature: no DIOR# with no chip select (an ISA card passes
//     every I/O read the PC makes to the IDE lines; a PCI controller none);
//   - no read the host failed to wait for;
//   - proof that it does wait: `min_long_reads` reads held long (longer than
//     any host's strobe, so a host that didn't wait would have been caught);
//   - past detection: `min_sectors` data sectors served, so the BIOS's and
//     the operating system's probes, in their least careful timing, are over.
// A host caught as ISA or not waiting stays on early answers until the next
// reset. Plain C++, no SDK: host-tested (test_auto_bus.cpp).
#pragma once

#include <cstdint>

namespace auto_bus {

struct Config {
    uint32_t min_sectors = 1000;    // data sectors served since the reset
    uint32_t min_long_reads = 20;   // reads held long, all waited for
};

// What core 0 knows, all counted since boot (the advisor takes differences).
struct Counters {
    bool dma_offered = false;
    bool no_cs_known = false;  // the bus observer runs with no DMA offered
    uint64_t dior_no_cs = 0;
    bool iordy_known = false;  // iordy_timer and iordy_check run
    uint32_t iordy_ignored = 0;
    uint32_t long_reads = 0;   // reads IORDY held long, the host waiting
    uint32_t sectors = 0;      // data sectors served (read or written)
    bool cdrom = false;        // a CD-ROM configured: the lane doesn't serve one
};

enum class Choice : uint8_t { Early, Lane };

// Why the advice is what it is: the first rule not met, or Ok for the lane.
enum class Why : uint8_t { Cdrom, Dma, Unknown, Isa, NotWaited, NoProof, Detecting, Ok };

const char* whyName(Why why);

class Advisor {
public:
    explicit Advisor(Config config = {}) : config_(config) {}

    // A reset: the host may re-detect in its slowest timing; start over.
    void reset(const Counters& now, uint32_t now_ms);

    // The latest counters; true if the advice changed.
    bool update(const Counters& now, uint32_t now_ms);

    Choice choice() const { return choice_; }
    Why why() const { return why_; }
    uint32_t sinceResetMs(uint32_t now_ms) const { return now_ms - reset_ms_; }
    uint32_t changedAtMs() const { return changed_ms_ - reset_ms_; }  // since the reset
    uint32_t sectors() const { return sectors_; }      // since the reset, at the last update
    uint32_t longReads() const { return long_reads_; }

private:
    Config config_;
    Counters base_{};
    uint32_t reset_ms_ = 0;
    uint32_t changed_ms_ = 0;
    bool caught_ = false;  // ISA or not waiting since the reset: early until the next
    Why caught_why_ = Why::Ok;
    uint32_t sectors_ = 0, long_reads_ = 0;
    Choice choice_ = Choice::Early;
    Why why_ = Why::Detecting;
};

}  // namespace auto_bus
