// Checks the data the host actually took off the bus against the generated
// disk, from the bus observer's samples (bus_observer.pio): one per read,
// taken just before DIOR# rose. Pure C++, host-tested.
//
// Reads of the data register, and multiword DMA's read strobes (DIOR# with
// no chip select: the observer records every DIOR#), are gathered into
// sectors of 256 words; any other read (the host polls status between
// sectors, and after each DMA command) starts a new one. A DMA transfer is
// whole sectors back to back, so they stay in step. A
// sector is recognised as generated sector N from its first words, which
// hold N and ~N (gen_disk.h), and then compared word by word.
//
// IDENTIFY blocks are checked too, against the drive's own (setIdentify), on
// the words that never change after start-up: serial, firmware, model and
// capacity, which is what a host names the disk from. A block is taken for
// IDENTIFY when at least IDENTIFY_MIN_MATCHES of those words match, so one
// with most of them wrong is still caught.
//
// Data from an SD image can't name itself, so it's checked against copies
// of the sectors served (expect()): the last EXPECT_SLOTS of them. A block
// matching one is good, and older copies the host never read (a command
// it abandoned, samples dropped) are let go; a block matching none is
// reported against the closest. With no copies waiting, blocks are skipped.
#pragma once

#include <cstdint>

namespace sector_check {

// One observer word: bits 0-20 as in the pin sample (DD0-15, DA2-0, CS0#,
// CS1#), bits 21-31 a sequence number that counts down by one per read.
constexpr unsigned SEQ_SHIFT = 21;
constexpr uint32_t SEQ_MASK = 0x7FFu;

// Mismatch::lba for an IDENTIFY block.
constexpr uint32_t IDENTIFY_LBA = 0xFFFFFFFFu;
constexpr unsigned IDENTIFY_MIN_MATCHES = 4;

// Copies of served sectors kept for checking image data (expect()). Core 1
// presents one sector at a time, so the host is never more than a couple
// behind core 0.
constexpr unsigned EXPECT_SLOTS = 16;

struct Mismatch {
    uint32_t lba;          // or IDENTIFY_LBA
    uint16_t first_word;   // index of the first wrong word
    uint16_t seen;         // what was on the bus there
    uint16_t want;
    uint16_t bad_words;    // how many of the 256 were wrong
    uint16_t bits_lost;    // 1s read as 0, over the sector
    uint16_t bits_gained;  // 0s read as 1
};

class Checker {
public:
    explicit Checker(uint32_t disk_sectors) : disk_sectors_(disk_sectors) {}

    // The drive's IDENTIFY block, to check IDENTIFY reads against. Copied.
    void setIdentify(const uint16_t words[256]);

    // A sector just served from an image, in bus order: the next data block
    // on the bus should be it (or a later one). Copied.
    void expect(uint32_t lba, const uint16_t words[256]);

    // Returns true if this read completed a sector (or IDENTIFY block) with
    // wrong words; the details are then in mismatch(). With `data` false
    // (the disk isn't the generated one) data is checked against expect()'s
    // copies, if there are any.
    bool observe(uint32_t word, bool data = true);

    const Mismatch& mismatch() const { return mismatch_; }
    // False after resync() until a read that isn't data, or alignAt(): until
    // then a caller may skip data reads without observing them.
    bool aligned() const { return aligned_; }

    // Whether four data words are the start of a generated sector (its LBA,
    // then the LBA inverted): how a caller that skipped reads finds a sector
    // start in the middle of a DMA command. Then call alignAt() and observe
    // from the first of them.
    bool isSectorStart(uint16_t w0, uint16_t w1, uint16_t w2, uint16_t w3) const {
        return w2 == uint16_t(~w0) && w3 == uint16_t(~w1) && (uint32_t(w0) | (uint32_t(w1) << 16)) < disk_sectors_;
    }
    void alignAt() {
        have_seq_ = false;
        count_ = 0;
        aligned_ = true;
    }
    uint32_t sectorsChecked() const { return checked_; }
    uint32_t sectorsBad() const { return bad_; }
    uint32_t sectorsSkipped() const { return skipped_; }
    uint32_t identifyChecked() const { return identify_checked_; }
    uint32_t identifyBad() const { return identify_bad_; }
    uint32_t gaps() const { return gaps_; }  // reads missed (sequence jumps)
    uint64_t dmaWords() const { return dma_words_; }  // DMA read words seen (data checked as any other)

    // After reads were discarded on purpose: start afresh, without a gap.
    // Within a DMA command only the word count says where sectors start,
    // so checking resumes after the next read that isn't data (the status
    // poll that ends the command).
    void resync() {
        have_seq_ = false;
        count_ = 0;
        aligned_ = false;
    }

    uint32_t expectMissed() const { return expect_missed_; }  // served copies never seen on the bus

    void resetCounts() {
        checked_ = bad_ = skipped_ = gaps_ = identify_checked_ = identify_bad_ = expect_missed_ = 0;
        dma_words_ = 0;
    }

private:
    bool finishSector(bool data);
    bool checkIdentify(bool& is_identify);
    bool matchesExpected();
    bool checkExpected();
    Mismatch compare(uint32_t lba, const uint16_t* want) const;

    uint32_t disk_sectors_;
    uint16_t identify_[256];
    bool have_identify_ = false;
    uint32_t identify_checked_ = 0, identify_bad_ = 0;
    uint16_t words_[256];
    unsigned count_ = 0;
    bool aligned_ = true;  // count_ is known to start at a sector (see resync)
    uint64_t dma_words_ = 0;  // 64-bit: DMA's rate wraps 32 bits in a few hours
    bool have_seq_ = false;
    uint32_t last_seq_ = 0;
    Mismatch mismatch_{};
    // expect()'s copies, a ring: expect_first_ the oldest of expect_count_.
    struct Expected {
        uint32_t lba;
        uint16_t words[256];
    };
    Expected expected_[EXPECT_SLOTS];
    unsigned expect_first_ = 0, expect_count_ = 0;
    uint32_t expect_missed_ = 0;
    uint32_t checked_ = 0, bad_ = 0, skipped_ = 0, gaps_ = 0;
};

}  // namespace sector_check
