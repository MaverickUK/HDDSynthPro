// A folder served as a read-only disk (docs/two-drives.md): a FAT16 volume
// laid out at start-up from a list of the folder's files and subfolders,
// with every sector computed when it's read, as the welcome disk's are. The
// partition table, boot record, FATs and folders come from the list; a
// file's sectors are read from the file itself through a FolderSource (the
// SD card on the drive, a real directory in the host tool).
//
// Each file and folder gets one unbroken run of clusters, in the order they
// were listed, so the FAT needs no storing: entry n is n + 1 inside a run,
// end of chain at its end. A folder's children are listed together, so each
// folder's entries are one range of the list. Pure C++, host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "block_source.h"

namespace folder_disk {

constexpr uint32_t SECTOR_BYTES = 512;
constexpr uint16_t ROOT = 0xFFFF;  // the folder served, as a parent
// The root folder holds 512 entries, as DOS formats a hard disk: the volume
// label and up to 511 files and folders.
constexpr uint32_t ROOT_ENTRIES = 512;
constexpr uint32_t ROOT_CHILDREN_MAX = ROOT_ENTRIES - 1;
// The disk's CHS layout, as defaultGeometry() reports for it.
constexpr uint32_t HEADS = 16;
constexpr uint32_t SECTORS_PER_TRACK = 63;
constexpr uint32_t PART_START = SECTORS_PER_TRACK;  // the partition starts on the second track

// One file or folder, 32 bytes. The name is FAT's 8.3, made from the
// source's long name (shortName()).
struct Entry {
    char name[11];  // "README  TXT"
    uint8_t attr;   // ATTR_DIR or ATTR_FILE
    uint16_t date;  // FAT date and time of the last write
    uint16_t time;
    union {
        uint32_t size;  // a file's bytes
        struct {
            uint16_t first_child;  // a folder's children: entries [first_child, first_child + children)
            uint16_t children;
        } dir;
    };
    uint32_t first_cluster;  // from layout; a file with no data has none (0 in its entry)
    uint16_t parent;         // the folder it's in, or ROOT
    uint16_t reserved;
    uint32_t locator;        // the source's handle for it (its index in its folder, on the card)
};
static_assert(sizeof(Entry) == 32, "an entry is 32 bytes");
constexpr uint8_t ATTR_READ_ONLY = 0x01;
constexpr uint8_t ATTR_DIR = 0x10;
constexpr uint8_t ATTR_FILE = 0x20;  // archive: an ordinary file

// What a folder holds, as the source lists it.
struct Item {
    const char* name;  // its long name
    bool dir;
    uint64_t size;  // bytes, for a file
    uint16_t date;
    uint16_t time;
    uint32_t locator;
};

// Where the folder's contents come from.
class FolderSource {
public:
    virtual ~FolderSource() = default;
    // Calls add(item) for each file and folder in `folder` (ROOT, or a
    // folder entry listed before); returns false if it couldn't list it.
    // add() returns false when the list is full: stop then.
    class Adder {
    public:
        virtual ~Adder() = default;
        virtual bool add(const Item& item) = 0;
    };
    virtual bool list(uint16_t folder, const Entry* entries, Adder& add) = 0;
    // Reads `bytes` (at most 512) of file `entry` from `offset`.
    virtual bool read(uint16_t entry, const Entry* entries, uint32_t offset, uint8_t* out, uint32_t bytes) = 0;
};

// FAT's 8.3 name for `long_name` (`out`: 11 characters, space-padded), as
// Windows makes them: upper case, characters FAT can't hold as '_', the base
// cut to 8 and the extension (after the last dot) to 3. If that changed the
// name, or `taken` says it's in use in the folder, the base ends in ~1, ~2...
// `taken(name, context)` returns whether a sibling already has that name.
using Taken = bool (*)(const char name[11], void* context);
void shortName(const char* long_name, char out[11], Taken taken, void* context);

// Why build() stopped short of the whole folder, if it did.
enum class Truncated : uint8_t { No, Entries, RootFull, Size };

class Volume {
public:
    // Lists the folder from `source` into `entries` (`capacity` of them)
    // and lays the volume out. False if nothing could be listed.
    bool build(FolderSource& source, Entry* entries, uint16_t capacity, const char* label);

    uint32_t sectors() const { return total_sectors_; }  // the whole disk, whole cylinders
    uint16_t count() const { return count_; }
    Truncated truncated() const { return truncated_; }
    uint32_t clusterBytes() const { return spc_ * SECTOR_BYTES; }
    uint32_t clusters() const { return clusters_; }
    uint32_t partStart() const { return PART_START; }
    uint32_t partSectors() const { return part_sectors_; }
    const Entry* entries() const { return entries_; }

    // Sector `lba` into `out`; false if a file's data couldn't be read.
    bool sector(uint32_t lba, uint8_t out[SECTOR_BYTES]);

private:
    bool listAll(FolderSource& source);
    bool layout();
    uint32_t clustersOf(const Entry& e) const;
    int entryAt(uint32_t cluster) const;  // the entry whose run holds `cluster`, or -1
    uint16_t fatEntry(uint32_t n) const;
    void putEntry(uint8_t* p, const Entry& e) const;
    void directorySector(uint16_t folder, uint32_t first_slot, uint8_t* out) const;
    void masterBootRecord(uint8_t* out) const;
    void bootRecord(uint8_t* out) const;

    FolderSource* source_ = nullptr;
    Entry* entries_ = nullptr;
    uint16_t capacity_ = 0;
    uint16_t count_ = 0;
    uint16_t root_children_ = 0;
    Truncated truncated_ = Truncated::No;
    char label_[11];
    uint32_t spc_ = 4;  // sectors per cluster
    uint32_t fat_sectors_ = 0;
    uint32_t clusters_ = 0;
    uint32_t part_sectors_ = 0;
    uint32_t total_sectors_ = 0;
    uint32_t fat_start_ = 0, root_start_ = 0, data_start_ = 0;
};

}  // namespace folder_disk

// The volume as the drive's disk: read-only, so write() refuses (ABRT).
class FolderDisk : public BlockSource {
public:
    explicit FolderDisk(folder_disk::Volume& volume) : volume_(volume) {}
    uint32_t sectors() const override { return volume_.sectors(); }
    bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= volume_.sectors()) return false;
        uint8_t bytes[folder_disk::SECTOR_BYTES];
        if (!volume_.sector(lba, bytes)) return false;
        for (int i = 0; i < 256; ++i) words[i] = uint16_t(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        return true;
    }

private:
    folder_disk::Volume& volume_;
};
