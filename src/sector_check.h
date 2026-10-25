// Checks the data the host actually took off the bus against the generated
// disk, from the bus observer's samples (bus_observer.pio): one per read,
// taken just before DIOR# rose. Pure C++, host-tested.
//
// Reads of the data register are gathered into sectors of 256 words; any
// other read (the host polls status between sectors) starts a new one. A
// sector is recognised as generated sector N from its first words, which
// hold N and ~N (gen_disk.h), and then compared word by word. Sectors that
// aren't generated (IDENTIFY, data the host wrote) are skipped.
#pragma once

#include <cstdint>

namespace sector_check {

// One observer word: bits 0-20 as in the pin sample (DD0-15, DA2-0, CS0#,
// CS1#), bits 21-31 a sequence number that counts down by one per read.
constexpr unsigned SEQ_SHIFT = 21;
constexpr uint32_t SEQ_MASK = 0x7FFu;

struct Mismatch {
    uint32_t lba;
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

    // Returns true if this read completed a sector with wrong words; the
    // details are then in mismatch().
    bool observe(uint32_t word);

    const Mismatch& mismatch() const { return mismatch_; }
    uint32_t sectorsChecked() const { return checked_; }
    uint32_t sectorsBad() const { return bad_; }
    uint32_t sectorsSkipped() const { return skipped_; }
    uint32_t gaps() const { return gaps_; }  // reads missed (sequence jumps)

    void resetCounts() { checked_ = bad_ = skipped_ = gaps_ = 0; }

private:
    bool finishSector();

    uint32_t disk_sectors_;
    uint16_t words_[256];
    unsigned count_ = 0;
    bool have_seq_ = false;
    uint32_t last_seq_ = 0;
    Mismatch mismatch_{};
    uint32_t checked_ = 0, bad_ = 0, skipped_ = 0, gaps_ = 0;
};

}  // namespace sector_check
