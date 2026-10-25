// The 256-word IDENTIFY DEVICE block (ATA/ATA-4 layout). Pure C++.
//
// It advertises a deliberately plain drive: LBA28 plus a default CHS
// geometry, PIO mode 0 only, IORDY supported, no DMA, no READ/WRITE MULTIPLE,
// no 48-bit addressing. It does declare a write cache and FLUSH CACHE: the
// storage behind it (SdFat, the card) buffers writes, so hosts should flush
// before power goes.
#pragma once

#include <cstdint>

struct Geometry {
    uint16_t cylinders;
    uint16_t heads;
    uint16_t sectors_per_track;
    uint32_t sectors() const { return uint32_t(cylinders) * heads * sectors_per_track; }
};

// A conventional default geometry for a disk of `total_sectors`:
// 16 heads x 63 sectors where that gives at least two cylinders (capped at
// 16383), otherwise 2 x 16 for tiny disks. The CHS capacity can be a little
// smaller than the LBA capacity; LBA reaches every sector.
Geometry defaultGeometry(uint32_t total_sectors);

struct IdentifyInfo {
    const char* model;     // up to 40 characters
    const char* serial;    // up to 20 characters
    const char* firmware;  // up to 8 characters
    uint32_t total_sectors;  // LBA28 capacity
};

// Fills `words` (256 entries, host byte order: word n is what the host reads
// on the nth DATA access). Word 255 carries the integrity checksum.
void buildIdentify(const IdentifyInfo& info, uint16_t words[256]);

// Rewrites the "current CHS translation" words (54-58), after INITIALIZE
// DEVICE PARAMETERS, and fixes up the checksum. `words` must already hold a
// valid IDENTIFY block (from buildIdentify): the checksum is adjusted for the
// changed words only, because core 1 runs this while the host may be waiting.
void setCurrentGeometry(uint16_t words[256], const Geometry& g);

// Sets the "write cache enabled" bit (word 85 bit 5) after SET FEATURES 02h
// or 82h, and fixes up the checksum, as setCurrentGeometry does.
void setWriteCacheEnabled(uint16_t words[256], bool on);

// ATA strings are space-padded and stored two characters per word with the
// first character in the high byte. Writes `len_words` words.
void putAtaString(uint16_t* words, unsigned len_words, const char* text);
