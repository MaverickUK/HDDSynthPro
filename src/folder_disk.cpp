#include "folder_disk.h"

#include <cstdio>
#include <cstring>

namespace folder_disk {

namespace {

constexpr uint32_t VOLUME_ID = 0x48445346;  // "HDSF"; also the MBR's disk signature
constexpr uint32_t CYLINDER = HEADS * SECTORS_PER_TRACK;
constexpr uint32_t RESERVED_SECTORS = 1;  // the boot record
constexpr uint32_t FAT_COUNT = 2;
constexpr uint32_t ROOT_SECTORS = ROOT_ENTRIES * 32 / SECTOR_BYTES;
// FAT16 by its cluster count: fewer would make it FAT12, more FAT32.
constexpr uint32_t MIN_CLUSTERS = 4085;
constexpr uint32_t MAX_CLUSTERS = 65524;
// The most clusters files may take at the largest cluster size (32 KB),
// leaving room for the folders: a folder bigger than 2 GB is served cut short.
constexpr uint32_t FILE_CLUSTER_BUDGET = 65000;
constexpr uint32_t MAX_FILE_BYTES = 0x7FFFFFFF;  // FAT16 volumes stop at 2 GB

void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

// A BIOS that boots this disk runs the MBR's code: INT 18h, "no bootable
// disk" (or the next boot device), then halt.
const uint8_t NOT_BOOTABLE[] = {0xCD, 0x18, 0xEB, 0xFE};  // int 18h; jmp $

// A partition table entry's CHS address of `lba`; past cylinder 1023, the
// largest CHS address, as DOS's FDISK writes it (the LBA fields are exact).
void putChs(uint8_t* p, uint32_t lba) {
    uint32_t cylinder = lba / CYLINDER;
    uint32_t head = lba / SECTORS_PER_TRACK % HEADS;
    uint32_t sector = lba % SECTORS_PER_TRACK + 1;
    if (cylinder > 1023) {
        cylinder = 1023;
        head = HEADS - 1;
        sector = SECTORS_PER_TRACK;
    }
    p[0] = uint8_t(head);
    p[1] = uint8_t(sector | ((cylinder >> 8) << 6));
    p[2] = uint8_t(cylinder);
}

// Characters a short name may hold besides letters and digits.
bool fatChar(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return c != 0 && std::strchr("!#$%&'()-@^_`{}~", c) != nullptr;
}

// One character of a long name as a short name holds it: lower case raised,
// anything FAT can't hold as '_' (and `lossy` set).
char shortChar(char c, bool& lossy) {
    if (c >= 'a' && c <= 'z') return char(c - 'a' + 'A');
    if (fatChar(c)) return c;
    lossy = true;
    return '_';
}

uint32_t ceilDiv(uint64_t a, uint32_t b) { return uint32_t((a + b - 1) / b); }

}  // namespace

void shortName(const char* long_name, char out[11], Taken taken, void* context) {
    while (*long_name == '.' || *long_name == ' ') ++long_name;  // leading dots and spaces go
    const char* dot = std::strrchr(long_name, '.');
    bool lossy = false;
    char base[8], ext[3];
    size_t base_len = 0, ext_len = 0;
    for (const char* p = long_name; *p && p != dot; ++p) {
        if (*p == ' ' || *p == '.') {  // spaces and inner dots aren't kept
            lossy = true;
            continue;
        }
        if (base_len == 8) {
            lossy = true;
            break;
        }
        base[base_len++] = shortChar(*p, lossy);
    }
    if (dot) {
        for (const char* p = dot + 1; *p; ++p) {
            if (*p == ' ') {
                lossy = true;
                continue;
            }
            if (ext_len == 3) {
                lossy = true;
                break;
            }
            ext[ext_len++] = shortChar(*p, lossy);
        }
    }
    if (base_len == 0) {  // nothing left of the name
        base[base_len++] = '_';
        lossy = true;
    }
    std::memset(out, ' ', 11);
    std::memcpy(out + 8, ext, ext_len);
    std::memcpy(out, base, base_len);
    if (!lossy && !taken(out, context)) return;
    // NAME~1.EXT, NAME~2.EXT... as many digits as it takes.
    for (uint32_t n = 1; n < 1000000; ++n) {
        char tail[8];
        int tail_len = std::snprintf(tail, sizeof tail, "~%lu", (unsigned long)n);
        size_t keep = base_len < size_t(8 - tail_len) ? base_len : size_t(8 - tail_len);
        std::memset(out, ' ', 8);
        std::memcpy(out, base, keep);
        std::memcpy(out + keep, tail, size_t(tail_len));
        if (!taken(out, context)) return;
    }
}

// ---- Listing ----------------------------------------------------------------

namespace {

struct Siblings {
    const Entry* entries;
    uint16_t first, end;
};

bool siblingHas(const char name[11], void* context) {
    const Siblings& s = *static_cast<const Siblings*>(context);
    for (uint16_t i = s.first; i < s.end; ++i)
        if (std::memcmp(s.entries[i].name, name, 11) == 0) return true;
    return false;
}

}  // namespace

// Adds a folder's items to the list, after those already there.
class ListAdder : public FolderSource::Adder {
public:
    ListAdder(Entry* entries, uint16_t& count, uint16_t capacity, uint16_t parent, Truncated& truncated,
              uint32_t& file_clusters)
        : entries_(entries), count_(count), capacity_(capacity), parent_(parent), first_(count),
          truncated_(truncated), file_clusters_(file_clusters) {}

    bool add(const Item& item) override {
        if (count_ == capacity_) {
            truncated_ = Truncated::Entries;
            return false;
        }
        if (parent_ == ROOT && count_ - first_ >= int(ROOT_CHILDREN_MAX)) {
            truncated_ = Truncated::RootFull;
            return false;
        }
        if (!item.dir) {
            if (item.size > MAX_FILE_BYTES) return true;  // too big for FAT16: left out
            uint32_t clusters = ceilDiv(item.size, 32768);
            if (file_clusters_ + clusters > FILE_CLUSTER_BUDGET) {
                truncated_ = Truncated::Size;
                return true;  // left out; smaller files after it may still fit
            }
            file_clusters_ += clusters;
        }
        Entry& e = entries_[count_];
        std::memset(&e, 0, sizeof e);
        Siblings s{entries_, first_, count_};
        shortName(item.name, e.name, siblingHas, &s);
        e.attr = item.dir ? ATTR_DIR : uint8_t(ATTR_FILE | ATTR_READ_ONLY);
        e.date = item.date;
        e.time = item.time;
        if (!item.dir) e.size = uint32_t(item.size);
        e.parent = parent_;
        e.locator = item.locator;
        ++count_;
        return true;
    }

private:
    Entry* entries_;
    uint16_t& count_;
    uint16_t capacity_;
    uint16_t parent_;
    uint16_t first_;
    Truncated& truncated_;
    uint32_t& file_clusters_;
};

bool Volume::listAll(FolderSource& source) {
    uint32_t file_clusters = 0;
    {
        ListAdder add(entries_, count_, capacity_, ROOT, truncated_, file_clusters);
        if (!source.list(ROOT, entries_, add)) return false;
    }
    root_children_ = count_;
    // Each folder's children, folder by folder in list order, so every
    // folder's children are one range.
    for (uint16_t i = 0; i < count_; ++i) {
        if (!(entries_[i].attr & ATTR_DIR)) continue;
        uint16_t first = count_;
        ListAdder add(entries_, count_, capacity_, i, truncated_, file_clusters);
        source.list(i, entries_, add);  // a folder that can't be listed is served empty
        entries_[i].dir.first_child = first;
        entries_[i].dir.children = uint16_t(count_ - first);
    }
    return true;
}

// ---- Layout -----------------------------------------------------------------

uint32_t Volume::clustersOf(const Entry& e) const {
    uint32_t bytes = (e.attr & ATTR_DIR) ? (2u + e.dir.children) * 32u : e.size;
    return ceilDiv(bytes, spc_ * SECTOR_BYTES);
}

bool Volume::layout() {
    for (uint32_t spc = 4; spc <= 64; spc *= 2) {  // 2 KB clusters to 32 KB
        spc_ = spc;
        uint64_t needed = 0;
        for (uint16_t i = 0; i < count_; ++i) needed += clustersOf(entries_[i]);
        if (needed > MAX_CLUSTERS) continue;
        uint32_t clusters = needed < MIN_CLUSTERS + 16 ? MIN_CLUSTERS + 16 : uint32_t(needed);
        uint32_t fat = ceilDiv((uint64_t(clusters) + 2) * 2, SECTOR_BYTES);
        uint32_t used = PART_START + RESERVED_SECTORS + FAT_COUNT * fat + ROOT_SECTORS + clusters * spc;
        total_sectors_ = ceilDiv(used, CYLINDER) * CYLINDER;  // whole cylinders, as a BIOS sees the disk
        // Fill the last cylinder with clusters (free space), keeping FAT16's limit.
        for (int pass = 0; pass < 4; ++pass) {
            uint32_t room = total_sectors_ - PART_START - RESERVED_SECTORS - FAT_COUNT * fat - ROOT_SECTORS;
            uint32_t fill = room / spc;
            if (fill > MAX_CLUSTERS) fill = MAX_CLUSTERS;
            uint32_t fat_needed = ceilDiv((uint64_t(fill) + 2) * 2, SECTOR_BYTES);
            clusters = fill;
            if (fat_needed <= fat) break;
            fat = fat_needed;
        }
        fat_sectors_ = fat;
        clusters_ = clusters;
        fat_start_ = PART_START + RESERVED_SECTORS;
        root_start_ = fat_start_ + FAT_COUNT * fat_sectors_;
        data_start_ = root_start_ + ROOT_SECTORS;
        part_sectors_ = RESERVED_SECTORS + FAT_COUNT * fat_sectors_ + ROOT_SECTORS + clusters_ * spc_;
        uint32_t next = 2;
        for (uint16_t i = 0; i < count_; ++i) {
            entries_[i].first_cluster = next;
            next += clustersOf(entries_[i]);
        }
        return true;
    }
    return false;
}

bool Volume::build(FolderSource& source, Entry* entries, uint16_t capacity, const char* label) {
    source_ = &source;
    entries_ = entries;
    capacity_ = capacity;
    count_ = 0;
    truncated_ = Truncated::No;
    std::memset(label_, ' ', sizeof label_);
    bool lossy = false;
    for (size_t i = 0; label && label[i] && i < sizeof label_; ++i) label_[i] = shortChar(label[i], lossy);
    if (!listAll(source)) return false;
    return layout();
}

// ---- Sectors ----------------------------------------------------------------

int Volume::entryAt(uint32_t cluster) const {
    // The last entry starting at or before `cluster` (first clusters only
    // grow along the list), back past any with no clusters of their own.
    int lo = 0, hi = count_;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (entries_[mid].first_cluster <= cluster) lo = mid + 1;
        else hi = mid;
    }
    for (int i = lo - 1; i >= 0; --i) {
        uint32_t n = clustersOf(entries_[i]);
        if (n == 0) continue;
        return cluster < entries_[i].first_cluster + n ? i : -1;
    }
    return -1;
}

uint16_t Volume::fatEntry(uint32_t n) const {
    if (n == 0) return 0xFFF8;  // the media byte, F8h
    if (n == 1) return 0xFFFF;  // end of chain; clean, no disk errors
    int i = entryAt(n);
    if (i < 0) return 0;  // free
    uint32_t end = entries_[i].first_cluster + clustersOf(entries_[i]);
    return n + 1 == end ? 0xFFFF : uint16_t(n + 1);
}

void Volume::putEntry(uint8_t* p, const Entry& e) const {
    std::memcpy(p, e.name, 11);
    p[11] = e.attr;
    put16(p + 0x0E, e.time);  // created
    put16(p + 0x10, e.date);
    put16(p + 0x12, e.date);  // last opened
    put16(p + 0x16, e.time);  // last written
    put16(p + 0x18, e.date);
    bool has_data = clustersOf(e) > 0;
    put16(p + 0x1A, has_data ? e.first_cluster : 0);
    put32(p + 0x1C, (e.attr & ATTR_DIR) ? 0 : e.size);
}

// 16 entries of folder `folder` (ROOT for the root) from `first_slot`.
void Volume::directorySector(uint16_t folder, uint32_t first_slot, uint8_t* out) const {
    for (uint32_t k = 0; k < SECTOR_BYTES / 32; ++k) {
        uint32_t slot = first_slot + k;
        uint8_t* p = out + 32 * k;
        if (folder == ROOT) {
            if (slot == 0) {
                std::memcpy(p, label_, 11);
                p[11] = 0x08;  // the volume label
                continue;
            }
            if (slot - 1 < root_children_) putEntry(p, entries_[slot - 1]);
            continue;
        }
        const Entry& dir = entries_[folder];
        if (slot < 2) {  // "." and ".."
            std::memset(p, ' ', 11);
            p[0] = '.';
            if (slot == 1) p[1] = '.';
            p[11] = ATTR_DIR;
            put16(p + 0x16, dir.time);
            put16(p + 0x18, dir.date);
            uint32_t cluster = slot == 0 ? dir.first_cluster
                               : dir.parent == ROOT ? 0  // ".." of a folder in the root
                                                    : entries_[dir.parent].first_cluster;
            put16(p + 0x1A, cluster);
            continue;
        }
        if (slot - 2 < dir.dir.children) putEntry(p, entries_[dir.dir.first_child + slot - 2]);
    }
}

void Volume::masterBootRecord(uint8_t* out) const {
    std::memcpy(out, NOT_BOOTABLE, sizeof NOT_BOOTABLE);
    put32(out + 0x1B8, VOLUME_ID);
    uint8_t* entry = out + 0x1BE;
    entry[0] = 0x00;  // not active: a data disk
    putChs(entry + 1, PART_START);
    entry[4] = 0x06;  // FAT16, 32 MB or more (DOS 3.31 and later)
    putChs(entry + 5, PART_START + part_sectors_ - 1);
    put32(entry + 8, PART_START);
    put32(entry + 12, part_sectors_);
    out[510] = 0x55;
    out[511] = 0xAA;
}

void Volume::bootRecord(uint8_t* out) const {
    static const uint8_t JUMP[] = {0xEB, 0x3C, 0x90};
    std::memcpy(out, JUMP, sizeof JUMP);
    std::memcpy(out + 3, "MSWIN4.1", 8);
    put16(out + 0x0B, SECTOR_BYTES);
    out[0x0D] = uint8_t(spc_);
    put16(out + 0x0E, RESERVED_SECTORS);
    out[0x10] = FAT_COUNT;
    put16(out + 0x11, ROOT_ENTRIES);
    put16(out + 0x13, part_sectors_ < 0x10000 ? part_sectors_ : 0);
    out[0x15] = 0xF8;  // a fixed disk
    put16(out + 0x16, fat_sectors_);
    put16(out + 0x18, SECTORS_PER_TRACK);
    put16(out + 0x1A, HEADS);
    put32(out + 0x1C, PART_START);
    put32(out + 0x20, part_sectors_ < 0x10000 ? 0 : part_sectors_);
    out[0x24] = 0x81;  // BIOS drive number: the second hard disk
    out[0x26] = 0x29;
    put32(out + 0x27, VOLUME_ID);
    std::memcpy(out + 0x2B, label_, 11);
    std::memcpy(out + 0x36, "FAT16   ", 8);
    std::memcpy(out + 0x3E, NOT_BOOTABLE, sizeof NOT_BOOTABLE);
    out[510] = 0x55;
    out[511] = 0xAA;
}

bool Volume::sector(uint32_t lba, uint8_t out[SECTOR_BYTES]) {
    std::memset(out, 0, SECTOR_BYTES);
    if (lba == 0) {
        masterBootRecord(out);
    } else if (lba == PART_START) {
        bootRecord(out);
    } else if (lba >= fat_start_ && lba < root_start_) {
        uint32_t first = (lba - fat_start_) % fat_sectors_ * (SECTOR_BYTES / 2);
        for (uint32_t i = 0; i < SECTOR_BYTES / 2; ++i) put16(out + 2 * i, fatEntry(first + i));
    } else if (lba >= root_start_ && lba < data_start_) {
        directorySector(ROOT, (lba - root_start_) * (SECTOR_BYTES / 32), out);
    } else if (lba >= data_start_ && lba < PART_START + part_sectors_) {
        uint32_t cluster = 2 + (lba - data_start_) / spc_;
        int i = entryAt(cluster);
        if (i < 0) return true;  // free space: zeros
        const Entry& e = entries_[i];
        uint32_t offset = (cluster - e.first_cluster) * spc_ * SECTOR_BYTES + (lba - data_start_) % spc_ * SECTOR_BYTES;
        if (e.attr & ATTR_DIR) {
            directorySector(uint16_t(i), offset / 32, out);
        } else if (offset < e.size) {
            uint32_t left = e.size - offset;
            return source_->read(uint16_t(i), entries_, offset, out, left < SECTOR_BYTES ? left : SECTOR_BYTES);
        }
    }
    return true;
}

}  // namespace folder_disk
