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
// written sector reads back. Holds as many distinct sectors as the storage
// given to it (setStorage()); a write to a new sector when all are in use
// fails (the host sees a write error) rather than losing data. With none it
// is read-only, as the CD-ROM's test disc uses it.
//
// The storage comes from the caller, not from a member array: the drive
// takes it from the heap at boot, and only when this disk is served, so an
// SD image leaves its 264 KB free (cfg::RAM_DISK_SECTORS, disks.cpp).
class RamOverlayDisk : public BlockSource {
public:
    // Bytes of storage per sector held: its LBA and its 256 words.
    static constexpr size_t SLOT_BYTES = sizeof(uint32_t) + 256 * sizeof(uint16_t);

    explicit RamOverlayDisk(BlockSource& base) : base_(&base) {}
    // The disk underneath, chosen at boot: only before anything is written.
    void setBase(BlockSource& base) { base_ = &base; }
    // Storage for `bytes / SLOT_BYTES` sectors, 4-byte aligned (malloc's is),
    // kept for good: only before anything is written.
    void setStorage(void* storage, size_t bytes) {
        slots_ = storage ? bytes / SLOT_BYTES : 0;
        lbas_ = static_cast<uint32_t*>(storage);
        data_ = reinterpret_cast<uint16_t(*)[256]>(lbas_ + slots_);
        used_ = 0;
    }
    uint32_t sectors() const override { return base_->sectors(); }

    CORE0_RAM bool read(uint32_t lba, uint16_t words[256]) override {
        int slot = find(lba);
        if (slot < 0) return base_->read(lba, words);
        std::memcpy(words, data_[slot], sizeof data_[slot]);
        // 0.35.1's size, so core 0's RAM code after it keeps its RAM banks
        // (scripts/check_core1_layout.py): only for sectors written.
        __asm volatile("nop\n nop\n nop\n nop");
        return true;
    }

    CORE0_RAM bool write(uint32_t lba, const uint16_t words[256]) override {
        if (lba >= sectors()) return false;
        int slot = find(lba);
        if (slot < 0) {
            if (used_ == slots_) {  // full, or no storage
                __asm volatile("nop\n nop\n nop\n nop\n nop\n nop");  // 0.35.1's size, as read()'s
                return false;
            }
            slot = int(used_++);
            lbas_[slot] = lba;
        }
        std::memcpy(data_[slot], words, sizeof data_[slot]);
        return true;
    }

    size_t used() const { return used_; }
    size_t slots() const { return slots_; }

private:
    CORE0_RAM int find(uint32_t lba) const {
        for (size_t i = 0; i < used_; ++i)
            if (lbas_[i] == lba) return int(i);
        return -1;
    }

    BlockSource* base_;
    uint32_t* lbas_ = nullptr;         // slots_ LBAs, then
    uint16_t (*data_)[256] = nullptr;  // slots_ sectors
    size_t slots_ = 0;
    size_t used_ = 0;
    // So that the objects after it move by whole 32-byte steps (the RAM
    // banks' period) against 0.35.1, whose overlay held its 264 KB itself.
    uint8_t bank_pad_[20] = {};
};

// An overlay with room for SLOTS sectors inside it: for the host tests (the
// drive's own takes its storage from the heap).
template <size_t SLOTS>
class FixedRamOverlayDisk : public RamOverlayDisk {
public:
    explicit FixedRamOverlayDisk(BlockSource& base) : RamOverlayDisk(base) { setStorage(storage_, sizeof storage_); }

private:
    alignas(4) uint8_t storage_[SLOTS * SLOT_BYTES];
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
