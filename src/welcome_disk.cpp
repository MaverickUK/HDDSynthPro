#include "welcome_disk.h"

#include <cstring>

namespace welcome_disk {

static void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

// A BIOS that boots this disk runs the MBR's code: INT 18h, "no bootable
// disk" (or the next boot device), then halt. Nothing on it boots.
static const uint8_t NOT_BOOTABLE[] = {0xCD, 0x18, 0xEB, 0xFE};  // int 18h; jmp $

// A partition table entry's CHS address of `lba`.
static void putChs(uint8_t* p, uint32_t lba) {
    uint32_t cylinder = lba / (HEADS * SECTORS_PER_TRACK);
    uint32_t head = lba / SECTORS_PER_TRACK % HEADS;
    uint32_t sector = lba % SECTORS_PER_TRACK + 1;
    p[0] = uint8_t(head);
    p[1] = uint8_t(sector | ((cylinder >> 8) << 6));
    p[2] = uint8_t(cylinder);
}

static void masterBootRecord(uint8_t* out) {
    std::memcpy(out, NOT_BOOTABLE, sizeof NOT_BOOTABLE);
    put32(out + 0x1B8, VOLUME_ID);  // the disk signature, which Windows uses
    uint8_t* entry = out + 0x1BE;   // the first of four
    entry[0] = 0x00;                // not active: a data disk
    putChs(entry + 1, PART_START);
    entry[4] = 0x06;  // FAT16, 32 MB or more (DOS 3.31 and later)
    putChs(entry + 5, PART_START + PART_SECTORS - 1);
    put32(entry + 8, PART_START);
    put32(entry + 12, PART_SECTORS);
    out[510] = 0x55;
    out[511] = 0xAA;
}

static void bootRecord(uint8_t* out) {
    static const uint8_t JUMP[] = {0xEB, 0x3C, 0x90};  // over the BPB to the code at 3Eh
    std::memcpy(out, JUMP, sizeof JUMP);
    std::memcpy(out + 3, "MSWIN4.1", 8);  // the OEM name the FAT spec recommends
    put16(out + 0x0B, SECTOR_BYTES);
    out[0x0D] = SECTORS_PER_CLUSTER;
    put16(out + 0x0E, RESERVED_SECTORS);
    out[0x10] = FAT_COUNT;
    put16(out + 0x11, ROOT_ENTRIES);
    put16(out + 0x13, PART_SECTORS < 0x10000 ? PART_SECTORS : 0);  // else the 32-bit count
    out[0x15] = 0xF8;  // a fixed disk
    put16(out + 0x16, FAT_SECTORS);
    put16(out + 0x18, SECTORS_PER_TRACK);
    put16(out + 0x1A, HEADS);
    put32(out + 0x1C, PART_START);  // hidden sectors: those before the partition
    put32(out + 0x20, PART_SECTORS < 0x10000 ? 0 : PART_SECTORS);
    out[0x24] = 0x80;  // BIOS drive number: the first hard disk
    out[0x26] = 0x29;  // the extended boot signature: the next three fields are there
    put32(out + 0x27, VOLUME_ID);
    std::memcpy(out + 0x2B, VOLUME_LABEL, 11);
    std::memcpy(out + 0x36, "FAT16   ", 8);
    std::memcpy(out + 0x3E, NOT_BOOTABLE, sizeof NOT_BOOTABLE);
    out[510] = 0x55;
    out[511] = 0xAA;
}

static uint32_t clustersOf(const File& f) { return (f.size + CLUSTER_BYTES - 1) / CLUSTER_BYTES; }

// FAT entry `n`: each file's clusters chained, the last marked end of chain.
static uint16_t fatEntry(uint32_t n, const File* files, size_t count) {
    if (n == 0) return 0xFFF8;  // the media byte, F8h
    if (n == 1) return 0xFFFF;  // end of chain; clean, no disk errors
    uint32_t first = 2;
    for (size_t i = 0; i < count; ++i) {
        uint32_t end = first + clustersOf(files[i]);
        if (n < end) return n + 1 == end ? 0xFFFF : uint16_t(n + 1);
        first = end;
    }
    return 0;  // free
}

// A directory entry's name: "README.TXT" as "README  TXT".
static void putName(uint8_t* p, const char* name) {
    std::memset(p, ' ', 11);
    const char* dot = std::strchr(name, '.');
    size_t base = dot ? size_t(dot - name) : std::strlen(name);
    std::memcpy(p, name, base < 8 ? base : 8);
    if (dot) {
        size_t ext = std::strlen(dot + 1);
        std::memcpy(p + 8, dot + 1, ext < 3 ? ext : 3);
    }
}

// Root folder entry `n`: the volume label, then the files.
static void putRootEntry(uint8_t* p, uint32_t n, const File* files, size_t count, uint16_t date) {
    if (n == 0) {
        std::memcpy(p, VOLUME_LABEL, 11);
        p[11] = 0x08;  // volume label
        put16(p + 0x18, date);
        return;
    }
    if (n > count) return;  // unused
    uint32_t first = 2;
    for (size_t i = 0; i + 1 < n; ++i) first += clustersOf(files[i]);
    const File& f = files[n - 1];
    putName(p, f.name);
    p[11] = 0x20;  // archive: an ordinary file
    put16(p + 0x10, date);  // created
    put16(p + 0x12, date);  // last opened
    put16(p + 0x18, date);  // last written (at midnight: time 0)
    put16(p + 0x1A, f.size ? first : 0);
    put32(p + 0x1C, f.size);
}

// The bytes of cluster sector `lba` (DATA_START or later) that hold a file.
static void dataSector(uint32_t lba, const File* files, size_t count, uint8_t* out) {
    uint32_t cluster = 2 + (lba - DATA_START) / SECTORS_PER_CLUSTER;
    uint32_t first = 2;
    for (size_t i = 0; i < count; ++i) {
        uint32_t end = first + clustersOf(files[i]);
        if (cluster < end) {
            uint32_t offset = (cluster - first) * CLUSTER_BYTES +
                              (lba - DATA_START) % SECTORS_PER_CLUSTER * SECTOR_BYTES;
            if (offset < files[i].size) {
                uint32_t left = files[i].size - offset;
                std::memcpy(out, files[i].data + offset, left < SECTOR_BYTES ? left : SECTOR_BYTES);
            }
            return;
        }
        first = end;
    }
}

void sector(uint32_t lba, const File* files, size_t count, uint16_t date, uint8_t out[SECTOR_BYTES]) {
    std::memset(out, 0, SECTOR_BYTES);
    if (lba == 0) {
        masterBootRecord(out);
    } else if (lba == PART_START) {
        bootRecord(out);
    } else if (lba >= FAT_START && lba < ROOT_START) {
        uint32_t first = (lba - FAT_START) % FAT_SECTORS * (SECTOR_BYTES / 2);
        for (uint32_t i = 0; i < SECTOR_BYTES / 2; ++i) put16(out + 2 * i, fatEntry(first + i, files, count));
    } else if (lba >= ROOT_START && lba < DATA_START) {
        uint32_t first = (lba - ROOT_START) * (SECTOR_BYTES / 32);
        for (uint32_t i = 0; i < SECTOR_BYTES / 32; ++i) putRootEntry(out + 32 * i, first + i, files, count, date);
    } else if (lba >= DATA_START && lba < PART_START + PART_SECTORS) {
        dataSector(lba, files, count, out);
    }
}

}  // namespace welcome_disk
