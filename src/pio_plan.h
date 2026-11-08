// Which PIO programs the drive starts, decided once at start-up from what the
// configuration needs (docs/pio-allocation.md, option C). The RP2350 has three
// PIO blocks of 32 instructions and 4 state machines each:
//
//   ide_bus      always, a block of its own (every variant fills 30-32 slots)
//   the card     4-bit SDIO (the PCB build) takes a whole block: our copy of
//                SdFat's driver (sdio_card) uses 27 slots and 2 state machines,
//                but only starts in an empty block (its GPIO base is 16)
//   the rest     multiword DMA's data phase (ide_dma), or IOCS16# for ISA
//                cards (iocs16), the diagnostics (iordy_timer with iordy_check,
//                bus_observer) and the read timing probe (bus_timing), as many
//                as fit in what's left
//
// With the card on SPI (the BB48 builds) there are two blocks besides ide_bus,
// as before: the diagnostics in one, ide_dma or iocs16 and bus_timing in the
// other. With the card on SDIO there is one, so something gives way:
//
//   DMA offered           ide_dma (32); no diagnostics: the fastest set-up
//   no DMA, isa = on      iocs16 and the IORDY timer (6 + 21); no bus observer
//   no DMA, isa = off     the IORDY timer and the bus observer (21 + 8)
//
// Pure C++, host-tested (test_pio_plan.cpp); the instruction counts are
// checked against the assembled programs by host/tests/test_pio_plan_sizes.py.
#pragma once

#include <cstdint>

namespace pio_plan {

// Instructions each program takes (its pioasm length), and state machines.
constexpr unsigned SLOTS = 32;
constexpr unsigned MACHINES = 4;
constexpr unsigned IDE_DMA_SLOTS = 17 + 15;       // ide_dma_read, ide_dma_write
constexpr unsigned IOCS16_SLOTS = 6;
constexpr unsigned IORDY_TIMER_SLOTS = 16 + 5;    // iordy_timer, iordy_check
constexpr unsigned BUS_OBSERVER_SLOTS = 8;
constexpr unsigned BUS_TIMING_SLOTS = 22;
constexpr unsigned SDIO_SLOTS = 10 + 5 + 4 + 4 + 4;  // sdio_card: cmd_rsp, rd_clk, rd_data, wr_data, wr_resp

struct Plan {
    bool ide_dma = false;
    bool iocs16 = false;
    bool iordy_timer = false;   // with iordy_check: slow reads, reads the host didn't wait for
    bool bus_observer = false;  // data and IDENTIFY checked on the cable
    bool bus_timing = false;    // the read timing probe (RDTIME)
    // Why the diagnostics are partly or wholly off, or nullptr if they're all on.
    const char* why = nullptr;
};

// `sdio`: the card runs over SDIO (taking a block); `dma`: multiword DMA is
// offered; `isa`: the host is an ISA card (isa = on), which needs IOCS16#.
inline Plan choose(bool sdio, bool dma, bool isa) {
    Plan p;
    if (!sdio) {  // two blocks: the diagnostics in one, the rest in the other
        p.iordy_timer = p.bus_observer = true;
        if (dma) {
            p.ide_dma = true;
        } else {
            p.iocs16 = true;  // ISA cards never use DMA; harmless on other hosts
            p.bus_timing = true;
        }
        return p;
    }
    if (dma) {
        p.ide_dma = true;
        p.why = "the card on SDIO and DMA offered: no PIO block left for them";
    } else if (isa) {
        p.iocs16 = p.iordy_timer = true;
        p.why = "the card on SDIO and isa = on: IOCS16# takes the bus observer's room";
    } else {
        p.iordy_timer = p.bus_observer = true;
        p.why = "the card on SDIO: no room for the read timing probe or IOCS16# (isa = off)";
    }
    return p;
}

// What the plan puts beside ide_bus, by block: the instructions and state
// machines each block needs (block 0 the card's when it's on SDIO). For the
// tests: every block must fit.
struct Use {
    unsigned slots = 0, machines = 0;
};
inline void blocks(const Plan& p, bool sdio, Use out[2]) {
    out[0] = out[1] = Use{};
    if (sdio) {
        out[0] = Use{SDIO_SLOTS, MACHINES};
        Use& b = out[1];
        if (p.ide_dma) b.slots += IDE_DMA_SLOTS, b.machines += 1;
        if (p.iocs16) b.slots += IOCS16_SLOTS, b.machines += 1;
        if (p.iordy_timer) b.slots += IORDY_TIMER_SLOTS, b.machines += 2;
        if (p.bus_observer) b.slots += BUS_OBSERVER_SLOTS, b.machines += 1;
        if (p.bus_timing) b.slots += BUS_TIMING_SLOTS, b.machines += 1;
        return;
    }
    Use& diag = out[0];
    if (p.iordy_timer) diag.slots += IORDY_TIMER_SLOTS, diag.machines += 2;
    if (p.bus_observer) diag.slots += BUS_OBSERVER_SLOTS, diag.machines += 1;
    Use& rest = out[1];
    if (p.ide_dma) rest.slots += IDE_DMA_SLOTS, rest.machines += 1;
    if (p.iocs16) rest.slots += IOCS16_SLOTS, rest.machines += 1;
    if (p.bus_timing) rest.slots += BUS_TIMING_SLOTS, rest.machines += 1;
}

}  // namespace pio_plan
