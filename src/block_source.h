// Where sector data comes from: one interface, a backend per slice.
// Called on core 0 only (it may be slow: the SD card in slice 5). Pure C++.
#pragma once

#include <cstdint>

#include "gen_disk.h"

class BlockSource {
public:
    virtual ~BlockSource() = default;
    virtual uint32_t sectors() const = 0;
    // Reads one sector as 256 bus words (the disk's byte 2n is word n's low
    // byte, as ATA transfers it). Returns false on a read error.
    virtual bool read(uint32_t lba, uint16_t words[256]) = 0;
};

// Slice 3: content computed from the LBA (gen_disk.h). No storage needed.
class GeneratedDisk : public BlockSource {
public:
    explicit GeneratedDisk(uint32_t sectors = gen_disk::DEFAULT_SECTORS) : sectors_(sectors) {}
    uint32_t sectors() const override { return sectors_; }
    bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= sectors_) return false;
        uint8_t bytes[gen_disk::SECTOR_BYTES];
        gen_disk::sector(lba, bytes);
        for (int i = 0; i < 256; ++i) words[i] = uint16_t(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        return true;
    }

private:
    uint32_t sectors_;
};
