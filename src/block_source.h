// Where sector data comes from: one interface, a backend per slice.
// Called on core 0 only (it may be slow: the SD card in slice 5). Pure C++.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gen_disk.h"

class BlockSource {
public:
    virtual ~BlockSource() = default;
    virtual uint32_t sectors() const = 0;
    // Reads one sector as 256 bus words (the disk's byte 2n is word n's low
    // byte, as ATA transfers it). Returns false on a read error.
    virtual bool read(uint32_t lba, uint16_t words[256]) = 0;
    // Writes one sector, in the same layout. Returns false on a write error;
    // a read-only source always does.
    virtual bool write(uint32_t lba, const uint16_t words[256]) {
        (void)lba;
        (void)words;
        return false;
    }
    // Makes everything written so far permanent (FLUSH CACHE). Returns false
    // if it couldn't. Nothing to do for a source without a cache.
    virtual bool flush() { return true; }
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

// Slice 4: writes kept in RAM on top of another disk (the welcome disk or
// the generated one), so the disk keeps its size and contents and every
// written sector reads back. Holds up to SLOTS distinct sectors; a write to a
// new sector when all are in use fails (the host sees a write error) rather
// than losing data.
template <size_t SLOTS>
class RamOverlayDisk : public BlockSource {
public:
    explicit RamOverlayDisk(BlockSource& base) : base_(&base) {}
    // The disk underneath, chosen at boot: only before anything is written.
    void setBase(BlockSource& base) { base_ = &base; }
    uint32_t sectors() const override { return base_->sectors(); }

    bool read(uint32_t lba, uint16_t words[256]) override {
        int slot = find(lba);
        if (slot < 0) return base_->read(lba, words);
        std::memcpy(words, data_[slot], sizeof data_[slot]);
        return true;
    }

    bool write(uint32_t lba, const uint16_t words[256]) override {
        if (lba >= sectors()) return false;
        int slot = find(lba);
        if (slot < 0) {
            if (used_ == SLOTS) return false;  // full
            slot = int(used_++);
            lbas_[slot] = lba;
        }
        std::memcpy(data_[slot], words, sizeof data_[slot]);
        return true;
    }

    size_t used() const { return used_; }

private:
    int find(uint32_t lba) const {
        for (size_t i = 0; i < used_; ++i)
            if (lbas_[i] == lba) return int(i);
        return -1;
    }

    BlockSource* base_;
    uint32_t lbas_[SLOTS];
    uint16_t data_[SLOTS][256];
    size_t used_ = 0;
};

// A disk image file, as ImageDisk sees it: byte-addressed reads and writes
// of a fixed-size file. On the drive it is HDDSYNTH.IMG on the SD card
// (sd_card.cpp); host tests use an in-memory one.
class ImageFile {
public:
    virtual ~ImageFile() = default;
    virtual uint64_t size() const = 0;
    virtual bool readAt(uint64_t offset, void* data, size_t bytes) = 0;
    virtual bool writeAt(uint64_t offset, const void* data, size_t bytes) = 0;
    virtual bool sync() { return true; }  // written data onto the medium
};

// Slice 5: the disk is an image file, sector n at byte n * 512. The file's
// bytes are the disk's bytes, and the bus words are stored little-endian, so
// a sector moves straight between the file and the 256-word buffer (byte 2n
// is word n's low byte, as ATA transfers it, on any little-endian CPU).
class ImageDisk : public BlockSource {
public:
    explicit ImageDisk(ImageFile& file) : file_(file) {}
    uint32_t sectors() const override { return uint32_t(file_.size() / gen_disk::SECTOR_BYTES); }

    bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= sectors()) return false;
        return file_.readAt(uint64_t(lba) * gen_disk::SECTOR_BYTES, words, gen_disk::SECTOR_BYTES);
    }

    bool write(uint32_t lba, const uint16_t words[256]) override {
        if (lba >= sectors()) return false;
        return file_.writeAt(uint64_t(lba) * gen_disk::SECTOR_BYTES, words, gen_disk::SECTOR_BYTES);
    }

    bool flush() override { return file_.sync(); }

private:
    ImageFile& file_;
};
