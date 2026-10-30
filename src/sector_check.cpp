#include "sector_check.h"

#include "bus_sample.h"
#include "gen_disk.h"

namespace sector_check {

// A sector with at most this many wrong words is a generated sector with
// errors; more, and it's other data that happens to start like one.
constexpr unsigned MAX_BAD_WORDS = 128;

static unsigned popcount16(uint16_t v) {
    unsigned n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// IDENTIFY's words that never change after start-up: serial (10-19),
// firmware and model (23-46), LBA28 capacity (60-61). The rest change with
// SET FEATURES and INITIALIZE DEVICE PARAMETERS (and the checksum with them).
static bool identifyWordIsStable(unsigned i) {
    return (i >= 10 && i <= 19) || (i >= 23 && i <= 46) || i == 60 || i == 61;
}

void Checker::setIdentify(const uint16_t words[256]) {
    for (unsigned i = 0; i < 256; ++i) identify_[i] = words[i];
    have_identify_ = true;
}

bool Checker::checkIdentify(bool& is_identify) {
    is_identify = false;
    if (!have_identify_) return false;
    Mismatch m{IDENTIFY_LBA, 0, 0, 0, 0, 0, 0};
    unsigned matches = 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (!identifyWordIsStable(i)) continue;
        uint16_t want = identify_[i];
        uint16_t seen = words_[i];
        if (seen == want) {
            if (want != 0) ++matches;  // zeros prove nothing
            continue;
        }
        if (m.bad_words == 0) {
            m.first_word = uint16_t(i);
            m.seen = seen;
            m.want = want;
        }
        ++m.bad_words;
        m.bits_lost = uint16_t(m.bits_lost + popcount16(want & ~seen));
        m.bits_gained = uint16_t(m.bits_gained + popcount16(seen & ~want));
    }
    if (matches < IDENTIFY_MIN_MATCHES) return false;
    is_identify = true;
    ++identify_checked_;
    if (m.bad_words == 0) return false;
    ++identify_bad_;
    mismatch_ = m;
    return true;
}

bool Checker::observe(uint32_t word, bool data) {
    uint32_t seq = (word >> SEQ_SHIFT) & SEQ_MASK;
    bool gap = have_seq_ && seq != ((last_seq_ - 1) & SEQ_MASK);
    have_seq_ = true;
    last_seq_ = seq;
    if (gap) {
        ++gaps_;
        count_ = 0;  // part of this sector is missing,
        aligned_ = false;  // and where the next one starts
    }
    uint32_t sample = word & ((1u << SEQ_SHIFT) - 1);
    bus::Block block = bus::block(sample);
    bool dma = block == bus::Block::None;  // DIOR# with no chip select: a DMA word
    if (!dma && (block != bus::Block::Command || bus::address(sample) != 0)) {
        count_ = 0;  // not the data register: the next data read starts a sector
        aligned_ = true;
        return false;
    }
    if (dma) ++dma_words_;
    words_[count_++] = bus::data(sample);
    if (count_ < 256) return false;
    count_ = 0;
    if (!aligned_) return false;  // perhaps two sectors' halves: not checked
    return finishSector(data);
}

bool Checker::finishSector(bool data) {
    bool is_identify;
    bool bad = checkIdentify(is_identify);
    if (is_identify) return bad;
    if (!data) {
        ++skipped_;
        return false;
    }
    // The LBA from words 0-1, or failing that from ~words 2-3.
    uint32_t candidates[2] = {
        uint32_t(words_[0]) | (uint32_t(words_[1]) << 16),
        ~(uint32_t(words_[2]) | (uint32_t(words_[3]) << 16)),
    };
    for (uint32_t lba : candidates) {
        if (lba >= disk_sectors_) continue;
        uint8_t bytes[gen_disk::SECTOR_BYTES];
        gen_disk::sector(lba, bytes);
        Mismatch m{lba, 0, 0, 0, 0, 0, 0};
        for (unsigned i = 0; i < 256; ++i) {
            uint16_t want = uint16_t(bytes[2 * i] | (bytes[2 * i + 1] << 8));
            uint16_t seen = words_[i];
            if (seen == want) continue;
            if (m.bad_words == 0) {
                m.first_word = uint16_t(i);
                m.seen = seen;
                m.want = want;
            }
            ++m.bad_words;
            m.bits_lost = uint16_t(m.bits_lost + popcount16(want & ~seen));
            m.bits_gained = uint16_t(m.bits_gained + popcount16(seen & ~want));
        }
        if (m.bad_words > MAX_BAD_WORDS) continue;
        ++checked_;
        if (m.bad_words == 0) return false;
        ++bad_;
        mismatch_ = m;
        return true;
    }
    ++skipped_;
    return false;
}

}  // namespace sector_check
