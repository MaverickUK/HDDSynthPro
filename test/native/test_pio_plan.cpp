// Host-side tests for pio_plan: which PIO programs start, and that each plan
// fits the RP2350's blocks.
#include "check.h"
#include "pio_plan.h"

using pio_plan::Plan;

static bool fits(const Plan& p, bool sdio) {
    pio_plan::Use use[2];
    pio_plan::blocks(p, sdio, use);
    for (const auto& u : use)
        if (u.slots > pio_plan::SLOTS || u.machines > pio_plan::MACHINES) return false;
    return true;
}

TEST(test_plan_spi_is_as_before) {
    // The BB48 builds: everything as before SDIO, all diagnostics on.
    Plan dma = pio_plan::choose(false, true, false);
    CHECK(dma.ide_dma && dma.iordy_timer && dma.bus_observer);
    CHECK(!dma.iocs16 && !dma.bus_timing);
    CHECK(dma.why == nullptr);
    for (bool isa : {false, true}) {
        Plan pio = pio_plan::choose(false, false, isa);
        CHECK(!pio.ide_dma && pio.iocs16 && pio.bus_timing && pio.iordy_timer && pio.bus_observer);
        CHECK(pio.why == nullptr);
    }
}

TEST(test_plan_sdio_with_dma_drops_the_diagnostics) {
    Plan p = pio_plan::choose(true, true, false);
    CHECK(p.ide_dma);
    CHECK(!p.iordy_timer && !p.bus_observer && !p.bus_timing && !p.iocs16);
    CHECK(p.why != nullptr);
}

TEST(test_plan_sdio_isa_keeps_iocs16_and_the_timer) {
    Plan p = pio_plan::choose(true, false, true);
    CHECK(p.iocs16 && p.iordy_timer);  // ISA cards need IOCS16#; the timer is what matters on them
    CHECK(!p.ide_dma && !p.bus_observer && !p.bus_timing);
    CHECK(p.why != nullptr);
}

TEST(test_plan_sdio_pio_keeps_the_diagnostics) {
    Plan p = pio_plan::choose(true, false, false);
    CHECK(p.iordy_timer && p.bus_observer);
    CHECK(!p.ide_dma && !p.iocs16 && !p.bus_timing);
}

TEST(test_plan_every_plan_fits) {
    for (bool sdio : {false, true})
        for (bool dma : {false, true})
            for (bool isa : {false, true}) CHECK(fits(pio_plan::choose(sdio, dma, isa), sdio));
}

TEST(test_plan_fit_check_catches_overflow) {
    Plan all;
    all.ide_dma = all.iocs16 = all.iordy_timer = all.bus_observer = all.bus_timing = true;
    CHECK(!fits(all, true));
    Plan both;
    both.iocs16 = both.iordy_timer = both.bus_observer = true;  // 35 slots beside SDIO
    CHECK(!fits(both, true));
}

void runPioPlanTests() {
    RUN(test_plan_spi_is_as_before);
    RUN(test_plan_sdio_with_dma_drops_the_diagnostics);
    RUN(test_plan_sdio_isa_keeps_iocs16_and_the_timer);
    RUN(test_plan_sdio_pio_keeps_the_diagnostics);
    RUN(test_plan_every_plan_fits);
    RUN(test_plan_fit_check_catches_overflow);
}
