// Host-side tests for auto_bus: which bus program the drive would choose
// (docs/auto-bus.md), from the counters core 0 keeps.
#include "auto_bus.h"
#include "check.h"

using auto_bus::Advisor;
using auto_bus::Choice;
using auto_bus::Counters;
using auto_bus::Why;

namespace {

// A host with every probe running, nothing seen yet.
Counters quiet() {
    Counters c;
    c.no_cs_known = true;
    c.iordy_known = true;
    return c;
}

// The Evesham after its benchmark: PCI, waits for IORDY.
Counters waitedThrough(Counters c, uint32_t sectors, uint32_t long_reads) {
    c.sectors += sectors;
    c.long_reads += long_reads;
    return c;
}

}  // namespace

TEST(auto_bus_a_host_that_waits_gets_the_lane_after_detection) {
    Advisor a;
    Counters c = quiet();
    a.reset(c, 0);
    CHECK(a.choice() == Choice::Early);
    CHECK(!a.update(waitedThrough(c, 10, 0), 100));  // the BIOS's IDENTIFY
    CHECK(a.why() == Why::NoProof);
    CHECK(!a.update(waitedThrough(c, 500, 30), 2000));  // DOS loading: proof, but still detecting
    CHECK(a.why() == Why::Detecting);
    CHECK(a.update(waitedThrough(c, 1000, 30), 5000));  // past detection
    CHECK(a.choice() == Choice::Lane);
    CHECK(a.why() == Why::Ok);
    CHECK_EQ(a.changedAtMs(), 5000u);
    CHECK(!a.update(waitedThrough(c, 5000, 90), 9000));  // stays
}

TEST(auto_bus_an_isa_card_never_gets_the_lane) {
    Advisor a;
    Counters c = quiet();
    a.reset(c, 0);
    c.dior_no_cs = 1;  // the ICL 386: millions of them, the first within ms
    CHECK(a.update(c, 50));
    CHECK(a.choice() == Choice::Early);
    CHECK(a.why() == Why::Isa);
    CHECK(!a.update(waitedThrough(c, 100000, 1000), 60000));  // however well it goes after
    CHECK(a.why() == Why::Isa);
}

TEST(auto_bus_a_host_that_stops_waiting_goes_back_and_stays) {
    Advisor a;
    Counters c = quiet();
    a.reset(c, 0);
    c = waitedThrough(c, 2000, 50);
    CHECK(a.update(c, 5000));
    CHECK(a.choice() == Choice::Lane);
    c.iordy_ignored = 3;  // Windows 95's driver in compatible timing, say
    CHECK(a.update(c, 6000));
    CHECK(a.choice() == Choice::Early);
    CHECK(a.why() == Why::NotWaited);
    CHECK(!a.update(waitedThrough(c, 10000, 500), 30000));
    CHECK(a.choice() == Choice::Early);
}

TEST(auto_bus_a_reset_starts_over) {
    Advisor a;
    Counters c = quiet();
    a.reset(c, 0);
    c.iordy_ignored = 2;  // the Pentium's BIOS detection, in compatible timing
    CHECK(a.update(c, 100));
    CHECK(a.why() == Why::NotWaited);
    a.reset(c, 1000);  // a RESET#: counts from here
    CHECK(a.why() == Why::Detecting);
    CHECK(a.update(waitedThrough(c, 1500, 25), 4000));
    CHECK(a.choice() == Choice::Lane);
    CHECK_EQ(a.changedAtMs(), 3000u);
    CHECK_EQ(a.sectors(), 1500u);
}

TEST(auto_bus_no_advice_without_the_evidence) {
    Advisor a;
    Counters c = quiet();
    c.no_cs_known = false;  // the card on SDIO with DMA offered: no observer
    a.reset(c, 0);
    a.update(waitedThrough(c, 5000, 100), 10000);
    CHECK(a.why() == Why::Unknown);
    CHECK(a.choice() == Choice::Early);
}

TEST(auto_bus_dma_and_a_cdrom_keep_early_answers) {
    Advisor a;
    Counters c = quiet();
    c.dma_offered = true;
    a.reset(c, 0);
    a.update(waitedThrough(c, 5000, 100), 10000);
    CHECK(a.why() == Why::Dma);
    c = quiet();
    c.cdrom = true;
    a.reset(c, 0);
    a.update(waitedThrough(c, 5000, 100), 10000);
    CHECK(a.why() == Why::Cdrom);
    CHECK(a.choice() == Choice::Early);
}

TEST(auto_bus_thresholds_come_from_the_config) {
    auto_bus::Config cfg;
    cfg.min_sectors = 10;
    cfg.min_long_reads = 1;
    Advisor a(cfg);
    Counters c = quiet();
    a.reset(c, 0);
    CHECK(a.update(waitedThrough(c, 10, 1), 10));
    CHECK(a.choice() == Choice::Lane);
    CHECK(std::string(auto_bus::whyName(a.why())) == "ok");
}

void runAutoBusTests() {
    RUN(auto_bus_a_host_that_waits_gets_the_lane_after_detection);
    RUN(auto_bus_an_isa_card_never_gets_the_lane);
    RUN(auto_bus_a_host_that_stops_waiting_goes_back_and_stays);
    RUN(auto_bus_a_reset_starts_over);
    RUN(auto_bus_no_advice_without_the_evidence);
    RUN(auto_bus_dma_and_a_cdrom_keep_early_answers);
    RUN(auto_bus_thresholds_come_from_the_config);
}
