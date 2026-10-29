// The welcome disk: what the drive serves when it starts with no microSD
// card, or with no image it can use. A 64 MB disk partitioned as a PC's
// would be, with one FAT16 volume, HDDSYNTHPRO, holding README.TXT (from
// welcome/README.TXT) and a sample HDDSYNTH.INI, so any computer (macOS,
// Windows, Linux, DOS 3.31 or later) shows how to set the drive up.
//
// Like the test disk (gen_disk.h), every sector is computed when it's read,
// so the disk costs no RAM: the partition table, the boot record, the FATs
// and the root folder from the layout below, the files' clusters from their
// text in flash, and zeros everywhere else. main.cpp keeps writes in the RAM
// overlay on top, until the drive restarts. Pure C++, host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "block_source.h"
#include "gen_disk.h"

namespace welcome_disk {

struct File {
    const char* name;  // 8.3, e.g. "README.TXT"
    const char* data;
    uint32_t size;     // bytes
};

// The layout, in LBAs.
constexpr uint32_t SECTOR_BYTES = gen_disk::SECTOR_BYTES;
constexpr uint32_t SECTORS = gen_disk::DEFAULT_SECTORS;  // 64 MB, the test disk's size
// defaultGeometry()'s (identify.cpp): what IDENTIFY tells a BIOS.
constexpr uint32_t HEADS = 16;
constexpr uint32_t SECTORS_PER_TRACK = 63;
constexpr uint32_t CYLINDERS = SECTORS / (HEADS * SECTORS_PER_TRACK);  // 130
// The partition starts on the second track, as DOS's FDISK leaves it, and
// ends with the last whole cylinder, the last sector a BIOS can reach.
constexpr uint32_t PART_START = SECTORS_PER_TRACK;
constexpr uint32_t PART_SECTORS = CYLINDERS * HEADS * SECTORS_PER_TRACK - PART_START;
// FAT16 as DOS formats 64 MB: 2 KB clusters, two FATs, 512 root entries.
constexpr uint32_t SECTORS_PER_CLUSTER = 4;
constexpr uint32_t CLUSTER_BYTES = SECTORS_PER_CLUSTER * SECTOR_BYTES;
constexpr uint32_t RESERVED_SECTORS = 1;  // the boot record
constexpr uint32_t FAT_COUNT = 2;
constexpr uint32_t ROOT_ENTRIES = 512;
constexpr uint32_t ROOT_SECTORS = ROOT_ENTRIES * 32 / SECTOR_BYTES;
// The FAT's size, by the FAT specification's formula ("FAT Volume
// Initialization"): just enough entries for every cluster.
constexpr uint32_t FAT_SECTORS =
    (PART_SECTORS - RESERVED_SECTORS - ROOT_SECTORS + 256 * SECTORS_PER_CLUSTER + FAT_COUNT - 1) /
    (256 * SECTORS_PER_CLUSTER + FAT_COUNT);
constexpr uint32_t FAT_START = PART_START + RESERVED_SECTORS;
constexpr uint32_t ROOT_START = FAT_START + FAT_COUNT * FAT_SECTORS;
constexpr uint32_t DATA_START = ROOT_START + ROOT_SECTORS;  // cluster 2
constexpr uint32_t CLUSTERS = (PART_START + PART_SECTORS - DATA_START) / SECTORS_PER_CLUSTER;
static_assert(CLUSTERS >= 4085 && CLUSTERS < 65525, "a FAT16 volume by its cluster count");
static_assert(FAT_SECTORS * SECTOR_BYTES / 2 >= CLUSTERS + 2, "the FAT has an entry for every cluster");

constexpr const char* VOLUME_LABEL = "HDDSYNTHPRO";  // 11 characters, the most FAT allows
constexpr uint32_t VOLUME_ID = 0x48445350;            // "HDSP"; also the MBR's disk signature

// Sector `lba` of the disk holding `files` (one after another from cluster 2,
// each dated `date` in FAT's format) into `out`. Zeros past the end.
void sector(uint32_t lba, const File* files, size_t count, uint16_t date, uint8_t out[SECTOR_BYTES]);

}  // namespace welcome_disk

class WelcomeDisk : public BlockSource {
public:
    WelcomeDisk(const welcome_disk::File* files, size_t count, uint16_t date)
        : files_(files), count_(count), date_(date) {}
    uint32_t sectors() const override { return welcome_disk::SECTORS; }
    bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= welcome_disk::SECTORS) return false;
        uint8_t bytes[welcome_disk::SECTOR_BYTES];
        welcome_disk::sector(lba, files_, count_, date_, bytes);
        for (int i = 0; i < 256; ++i) words[i] = uint16_t(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        return true;
    }

private:
    const welcome_disk::File* files_;
    size_t count_;
    uint16_t date_;
};
