// Tests for the bus observer's sector check (sector_check), with observer
// words built as bus_observer.pio produces them.
#include <cstdint>
#include <vector>

#include "bus_sample.h"
#include "check.h"
#include "config.h"
#include "gen_disk.h"
#include "identify.h"
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
    // A multiword DMA read strobe: DIOR# with neither chip select.
    uint32_t dma(uint16_t v) {
        uint32_t s = v | bus::maskOf(cfg::PIN_CS0_N) | bus::maskOf(cfg::PIN_CS1_N);
        uint32_t w = s | ((seq & SEQ_MASK) << SEQ_SHIFT);
        seq = (seq - 1) & SEQ_MASK;
        return w;
    }
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

TEST(test_sector_check_dma_sectors) {
    // READ DMA of three sectors: 768 strobes back to back, then the status poll.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    c.observe(o.status());
    int bad = 0;
    for (uint32_t lba = 9000; lba < 9003; ++lba)
        for (uint16_t v : sectorWords(lba)) bad += c.observe(o.dma(v));
    c.observe(o.status());
    CHECK_EQ(bad, 0);
    CHECK_EQ(c.sectorsChecked(), 3u);
    CHECK_EQ(c.dmaWords(), 768u);
}

TEST(test_sector_check_dma_lost_first_word) {
    // 0.16.1's bug: the first word of each DMA sector read 0048h.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    c.observe(o.status());
    auto w = sectorWords(777);
    w[0] = 0x0048;
    int bad = 0;
    for (uint16_t v : w) bad += c.observe(o.dma(v));
    CHECK_EQ(bad, 1);  // recognised from ~words 2-3, and caught
    CHECK_EQ(c.sectorsBad(), 1u);
    CHECK_EQ(c.mismatch().lba, 777u);
    CHECK_EQ(c.mismatch().first_word, 0);
    CHECK_EQ(c.mismatch().bad_words, 1);
}

TEST(test_sector_check_dma_then_pio) {
    // DMA and PIO reads of data alike; a status poll between keeps them apart.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    c.observe(o.status());
    for (uint16_t v : sectorWords(10)) c.observe(o.dma(v));
    CHECK_EQ(feed(c, o, sectorWords(11)), 0);  // status, then PIO data
    CHECK_EQ(c.sectorsChecked(), 2u);
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

static std::vector<uint16_t> identifyWords() {
    std::vector<uint16_t> w(256);
    buildIdentify(IdentifyInfo{"HDDSYNTH PROTO", "HSP-000001", "0.11.0", gen_disk::DEFAULT_SECTORS}, w.data());
    return w;
}

TEST(test_sector_check_identify_good) {
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    std::vector<uint16_t> id = identifyWords();
    c.setIdentify(id.data());
    CHECK_EQ(feed(c, o, id), 0);
    CHECK_EQ(c.identifyChecked(), 1u);
    CHECK_EQ(c.identifyBad(), 0u);
    CHECK_EQ(c.sectorsSkipped(), 0u);

    // Words that change after start-up (geometry, write cache, checksum) don't count.
    std::vector<uint16_t> changed = id;
    changed[54] ^= 0x10;
    changed[85] ^= 0x20;
    changed[255] ^= 0x1200;
    CHECK_EQ(feed(c, o, changed), 0);
    CHECK_EQ(c.identifyChecked(), 2u);
}

TEST(test_sector_check_identify_garbled) {
    // As the NS1081/JMicron once read it: the model came out "HD P", three
    // words in four read as zero.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    std::vector<uint16_t> id = identifyWords();
    c.setIdentify(id.data());
    std::vector<uint16_t> garbled = id;
    for (unsigned i = 0; i < 256; ++i)
        if (i % 4 != 3) garbled[i] = 0;
    CHECK_EQ(feed(c, o, garbled), 1);
    CHECK_EQ(c.identifyChecked(), 1u);
    CHECK_EQ(c.identifyBad(), 1u);
    const Mismatch& m = c.mismatch();
    CHECK_EQ(m.lba, IDENTIFY_LBA);
    CHECK_EQ(unsigned(m.first_word), 10u);  // the serial number's first word
    CHECK_EQ(unsigned(m.seen), 0u);
    CHECK(m.bad_words > 20);
    CHECK_EQ(unsigned(m.bits_gained), 0u);
}

TEST(test_sector_check_identify_without_data_check) {
    // On an SD image only IDENTIFY is checked; data sectors are skipped.
    Checker c(gen_disk::DEFAULT_SECTORS);
    Observer o;
    std::vector<uint16_t> id = identifyWords();
    c.setIdentify(id.data());
    std::vector<uint16_t> bad = id;
    bad[27] = 0;
    int reports = 0;
    for (uint32_t i = 0; i < 4; ++i) reports += feed(c, o, sectorWords(i));
    reports += feed(c, o, bad);
    for (uint32_t i = 0; i < 4; ++i) reports += feed(c, o, sectorWords(i));
    CHECK_EQ(reports, 1);
    CHECK_EQ(c.sectorsChecked(), 8u);
    CHECK_EQ(c.identifyBad(), 1u);

    Checker image(gen_disk::DEFAULT_SECTORS);
    image.setIdentify(id.data());
    int image_reports = 0;
    auto feed_image = [&](const std::vector<uint16_t>& words) {
        image_reports += image.observe(o.status(), false) ? 1 : 0;
        for (uint16_t w : words) image_reports += image.observe(o.data(w), false) ? 1 : 0;
    };
    feed_image(sectorWords(5));
    feed_image(bad);
    CHECK_EQ(image_reports, 1);
    CHECK_EQ(image.sectorsChecked(), 0u);
    CHECK_EQ(image.sectorsSkipped(), 1u);
    CHECK_EQ(image.identifyBad(), 1u);
}

void runSectorCheckTests() {
    RUN(test_sector_check_identify_good);
    RUN(test_sector_check_identify_garbled);
    RUN(test_sector_check_identify_without_data_check);
    RUN(test_sector_check_good_sectors);
    RUN(test_sector_check_overnight_mismatch);
    RUN(test_sector_check_lba_from_inverted_words);
    RUN(test_sector_check_skips_other_data);
    RUN(test_sector_check_dma_sectors);
    RUN(test_sector_check_dma_lost_first_word);
    RUN(test_sector_check_dma_then_pio);
    RUN(test_sector_check_sectors_split_by_status_reads);
    RUN(test_sector_check_gap_drops_the_sector);
    RUN(test_sector_check_sequence_wraps);
}
