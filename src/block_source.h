// Where sector data comes from: one interface, a backend per slice.
// Called on core 0 only (it may be slow: the SD card in slice 5). Pure C++.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core1_ram.h"  // CORE0_RAM: the reads and writes are core 0's per-sector code
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
    CORE0_RAM bool read(uint32_t lba, uint16_t words[256]) override {
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

    CORE0_RAM bool read(uint32_t lba, uint16_t words[256]) override {
        int slot = find(lba);
        if (slot < 0) return base_->read(lba, words);
        std::memcpy(words, data_[slot], sizeof data_[slot]);
        return true;
    }

    CORE0_RAM bool write(uint32_t lba, const uint16_t words[256]) override {
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

    size_t used() const { return used_ - lent_; }
    // The sectors it can hold: SLOTS, less any lent.
    size_t capacity() const { return SLOTS - lent_; }

    // Lends the start of its store, `bytes` rounded up to whole sectors, for
    // something else to keep there for good (the folder disk's list, as the
    // overlay is only written on the test disk: docs/two-drives.md); it then
    // holds that many fewer sectors. Only before anything is written, and
    // once: nullptr otherwise, or if it would leave none. 4-byte aligned.
    // The lent slots are held for LENT_LBA, which no disk has (write() refuses
    // it, and ATA can't address it), so read(), write() and find() are
    // unchanged: in core 0's RAM, their size sets where the code after them
    // sits, and so its RAM banks.
    void* lend(size_t bytes) {
        const size_t slots = (bytes + sizeof data_[0] - 1) / sizeof data_[0];
        if (used_ != 0 || slots == 0 || slots >= SLOTS) return nullptr;
        for (size_t i = 0; i < slots; ++i) lbas_[i] = LENT_LBA;
        used_ = lent_ = slots;
        return data_[0];
    }
    static constexpr uint32_t LENT_LBA = 0xFFFFFFFF;

private:
    CORE0_RAM int find(uint32_t lba) const {
        for (size_t i = 0; i < used_; ++i)
            if (lbas_[i] == lba) return int(i);
        return -1;
    }

    BlockSource* base_;
    uint32_t lbas_[SLOTS];
    uint16_t data_[SLOTS][256];
    size_t used_ = 0;
    size_t lent_ = 0;
    // To whole 32-byte steps with lent_: the objects after it keep their RAM
    // banks (docs/speed-log.md, 19 Nov 2026).
    uint8_t bank_pad_[32 - sizeof(size_t)];
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

    CORE0_RAM bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= sectors()) return false;
        return file_.readAt(uint64_t(lba) * gen_disk::SECTOR_BYTES, words, gen_disk::SECTOR_BYTES);
    }

    CORE0_RAM bool write(uint32_t lba, const uint16_t words[256]) override {
        if (lba >= sectors()) return false;
        return file_.writeAt(uint64_t(lba) * gen_disk::SECTOR_BYTES, words, gen_disk::SECTOR_BYTES);
    }

    bool flush() override { return file_.sync(); }

private:
    ImageFile& file_;
};
