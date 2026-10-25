// Tests for the bus observer's sector check (sector_check), with observer
// words built as bus_observer.pio produces them.
#include <cstdint>
#include <vector>

#include "bus_sample.h"
#include "check.h"
#include "config.h"
#include "gen_disk.h"
#include "sector_check.h"

using namespace sector_check;

namespace {

// Builds observer words, numbering them as the PIO program does.
struct Observer {
    uint32_t seq = 0;

    uint32_t read(bool command_block, unsigned addr, uint16_t data) {
        uint32_t s = data;
        s |= (addr & 7u) << bus::bitOf(cfg::PIN_DA0);
        // CS0# low selects the command block, CS1# low the control block
        s |= command_block ? bus::maskOf(cfg::PIN_CS1_N) : bus::maskOf(cfg::PIN_CS0_N);
        uint32_t w = s | ((seq & SEQ_MASK) << SEQ_SHIFT);
        seq = (seq - 1) & SEQ_MASK;
        return w;
    }
    uint32_t status() { return read(true, 7, 0x58); }
    uint32_t data(uint16_t v) { return read(true, 0, v); }
};

std::vector<uint16_t> sectorWords(uint32_t lba) {
    uint8_t bytes[gen_disk::SECTOR_BYTES];
    gen_disk::sector(lba, bytes);
    std::vector<uint16_t> w(256);
    for (unsigned i = 0; i < 256; ++i) w[i] = uint16_t(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    return w;
}

// Feeds a status poll and then the sector's words; returns how many
// observe() calls reported a mismatch.
int feed(Checker& c, Observer& o, const std::vector<uint16_t>& words) {
    int reports = c.observe(o.status()) ? 1 : 0;
    for (uint16_t w : words) reports += c.observe(o.data(w)) ? 1 : 0;
    return reports;
}

}  // namespace

TEST(test_sector_check_good_sectors) {
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    for (uint32_t lba : {0u, 1u, 107706u, gen_disk::DEFAULT_SECTORS - 1}) CHECK_EQ(feed(c, o, sectorWords(lba)), 0);
    CHECK_EQ(c.sectorsChecked(), 4u);
    CHECK_EQ(c.sectorsBad(), 0u);
    CHECK_EQ(c.gaps(), 0u);
}

TEST(test_sector_check_overnight_mismatch) {
    // The soak's bad sector (2026-10-24): words 2, 3, 5, 6 and 7 lost bits.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    auto w = sectorWords(107706);
    w[2] = 0x0000;
    w[3] = 0xff40;
    w[5] &= 0x00ff;
    w[6] &= 0x00ff;
    w[7] &= 0x00ff;
    CHECK_EQ(feed(c, o, w), 1);
    const Mismatch& m = c.mismatch();
    CHECK_EQ(m.lba, 107706u);
    CHECK_EQ(m.first_word, 2u);
    CHECK_EQ(m.seen, 0x0000);
    CHECK_EQ(m.want, 0x5b45);
    CHECK_EQ(m.bad_words, 5u);
    CHECK(m.bits_lost > 0);
    CHECK_EQ(m.bits_gained, 0u);
    CHECK_EQ(c.sectorsBad(), 1u);
}

TEST(test_sector_check_lba_from_inverted_words) {
    // Words 0-1 wrong: the LBA comes from ~words 2-3.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    auto w = sectorWords(5000);
    w[0] = 0;
    w[1] = 0x4000;
    CHECK_EQ(feed(c, o, w), 1);
    CHECK_EQ(c.mismatch().lba, 5000u);
    CHECK_EQ(c.mismatch().first_word, 0u);
    CHECK_EQ(c.mismatch().bad_words, 2u);
}

TEST(test_sector_check_skips_other_data) {
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    std::vector<uint16_t> written(256);
    for (unsigned i = 0; i < 256; ++i) written[i] = uint16_t(i * 2654435761u >> 7);
    CHECK_EQ(feed(c, o, written), 0);
    // Starts like generated sector 12 but the rest is other data
    written[0] = 12;
    written[1] = 0;
    CHECK_EQ(feed(c, o, written), 0);
    CHECK_EQ(c.sectorsSkipped(), 2u);
    CHECK_EQ(c.sectorsChecked(), 0u);
}

TEST(test_sector_check_sectors_split_by_status_reads) {
    // A sector cut short by a status read is dropped, not merged.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    auto w = sectorWords(42);
    c.observe(o.status());
    for (unsigned i = 0; i < 100; ++i) c.observe(o.data(w[i]));
    CHECK_EQ(feed(c, o, w), 0);
    CHECK_EQ(c.sectorsChecked(), 1u);
    CHECK_EQ(c.sectorsSkipped(), 0u);
}

TEST(test_sector_check_gap_drops_the_sector) {
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    auto w = sectorWords(77);
    c.observe(o.status());
    for (unsigned i = 0; i < 256; ++i) {
        if (i == 50) o.seq = (o.seq - 3) & SEQ_MASK;  // three reads lost
        CHECK(!c.observe(o.data(w[i])));
    }
    CHECK_EQ(c.gaps(), 1u);
    CHECK_EQ(c.sectorsChecked(), 0u);
    CHECK_EQ(c.sectorsBad(), 0u);
    CHECK_EQ(feed(c, o, w), 0);  // the next one is whole
    CHECK_EQ(c.sectorsChecked(), 1u);
}

TEST(test_sector_check_sequence_wraps) {
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    o.seq = 3;
    for (int k = 0; k < 20; ++k) feed(c, o, sectorWords(uint32_t(k)));
    CHECK_EQ(c.gaps(), 0u);
    CHECK_EQ(c.sectorsChecked(), 20u);
}

void runSectorCheckTests() {
    RUN(test_sector_check_good_sectors);
    RUN(test_sector_check_overnight_mismatch);
    RUN(test_sector_check_lba_from_inverted_words);
    RUN(test_sector_check_skips_other_data);
    RUN(test_sector_check_sectors_split_by_status_reads);
    RUN(test_sector_check_gap_drops_the_sector);
    RUN(test_sector_check_sequence_wraps);
}
