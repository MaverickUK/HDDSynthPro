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
// with most of them wrong is still caught. Other blocks (data from an SD
// image, data the host wrote) are skipped.
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

    // Returns true if this read completed a sector (or IDENTIFY block) with
    // wrong words; the details are then in mismatch(). With `data` false
    // (the disk isn't the generated one) only IDENTIFY blocks are checked.
    bool observe(uint32_t word, bool data = true);

    const Mismatch& mismatch() const { return mismatch_; }
    uint32_t sectorsChecked() const { return checked_; }
    uint32_t sectorsBad() const { return bad_; }
    uint32_t sectorsSkipped() const { return skipped_; }
    uint32_t identifyChecked() const { return identify_checked_; }
    uint32_t identifyBad() const { return identify_bad_; }
    uint32_t gaps() const { return gaps_; }  // reads missed (sequence jumps)
    uint32_t dmaWords() const { return dma_words_; }  // DMA read words seen (data checked as any other)

    // After reads were discarded on purpose: start afresh, without a gap.
    void resync() {
        have_seq_ = false;
        count_ = 0;
    }

    void resetCounts() { checked_ = bad_ = skipped_ = gaps_ = identify_checked_ = identify_bad_ = dma_words_ = 0; }

private:
    bool finishSector(bool data);
    bool checkIdentify(bool& is_identify);

    uint32_t disk_sectors_;
    uint16_t identify_[256];
    bool have_identify_ = false;
    uint32_t identify_checked_ = 0, identify_bad_ = 0;
    uint16_t words_[256];
    unsigned count_ = 0;
    uint32_t dma_words_ = 0;
    bool have_seq_ = false;
    uint32_t last_seq_ = 0;
    Mismatch mismatch_{};
    uint32_t checked_ = 0, bad_ = 0, skipped_ = 0, gaps_ = 0;
};

}  // namespace sector_check
