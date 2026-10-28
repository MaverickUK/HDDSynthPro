#include "identify.h"

#include <cstring>

#include "core1_ram.h"

CORE1_RAM Geometry defaultGeometry(uint32_t total_sectors) {
    const uint32_t big_track = 16u * 63u;
    if (total_sectors >= 2 * big_track) {
        uint32_t cyl = total_sectors / big_track;
        if (cyl > 16383) cyl = 16383;
        return Geometry{uint16_t(cyl), 16, 63};
    }
    uint32_t cyl = total_sectors / (2u * 16u);
    if (cyl == 0) cyl = 1;
    return Geometry{uint16_t(cyl), 2, 16};
}

void putAtaString(uint16_t* words, unsigned len_words, const char* text) {
    size_t len = std::strlen(text);
    for (unsigned i = 0; i < len_words; ++i) {
        size_t a = 2 * i, b = 2 * i + 1;
        uint8_t hi = a < len ? uint8_t(text[a]) : ' ';
        uint8_t lo = b < len ? uint8_t(text[b]) : ' ';
        words[i] = uint16_t((hi << 8) | lo);
    }
}

static void fixChecksum(uint16_t w[256]) {
    // Integrity word: signature A5h in the low byte, and a checksum in the
    // high byte that makes all 512 bytes sum to zero (mod 256).
    w[255] = 0x00A5;
    uint8_t sum = 0;
    for (int i = 0; i < 256; ++i) sum += uint8_t(w[i] & 0xFF) + uint8_t(w[i] >> 8);
    w[255] = uint16_t((uint8_t(-sum) << 8) | 0xA5);
}

static uint8_t byteSum(uint16_t word) { return uint8_t((word & 0xFF) + (word >> 8)); }

CORE1_RAM void setCurrentGeometry(uint16_t w[256], const Geometry& g) {
    uint32_t sectors = g.sectors();
    const uint16_t current[5] = {g.cylinders, g.heads, g.sectors_per_track,
                                 uint16_t(sectors & 0xFFFF), uint16_t(sectors >> 16)};
    // All 512 bytes sum to zero, so the checksum byte goes up by what leaves
    // the sum and down by what joins it.
    uint8_t checksum = uint8_t(w[255] >> 8);
    for (int i = 0; i < 5; ++i) {
        checksum += byteSum(w[54 + i]);
        w[54 + i] = current[i];
        checksum -= byteSum(current[i]);
    }
    w[255] = uint16_t((checksum << 8) | 0xA5);
}

CORE1_RAM void setWriteCacheEnabled(uint16_t w[256], bool on) {
    uint16_t word = on ? uint16_t(w[85] | (1u << 5)) : uint16_t(w[85] & ~(1u << 5));
    uint8_t checksum = uint8_t(uint8_t(w[255] >> 8) + byteSum(w[85]) - byteSum(word));
    w[85] = word;
    w[255] = uint16_t((checksum << 8) | 0xA5);
}

void buildIdentify(const IdentifyInfo& info, uint16_t w[256]) {
    std::memset(w, 0, 256 * sizeof(uint16_t));
    Geometry g = defaultGeometry(info.total_sectors);
    uint32_t chs_sectors = g.sectors();

    w[0] = 0x0040;  // fixed device, not removable
    w[1] = g.cylinders;
    w[3] = g.heads;
    w[6] = g.sectors_per_track;
    putAtaString(&w[10], 10, info.serial);
    putAtaString(&w[23], 4, info.firmware);
    putAtaString(&w[27], 20, info.model);
    w[47] = 0x0000;  // READ/WRITE MULTIPLE not supported
    w[49] = (1u << 11) | (1u << 9);  // IORDY supported, LBA supported (no DMA)
    w[50] = 0x4000;  // bit 14 shall be set
    // Modes 0-2 go in word 51's high byte (an obsolete field, but how they're
    // offered), up to 2; modes 3 and 4 are word 64's bits 0 and 1.
    uint8_t pio_mode = info.pio_mode > MAX_PIO_MODE ? MAX_PIO_MODE : info.pio_mode;
    w[51] = uint16_t((pio_mode > 2 ? 2 : pio_mode) << 8);
    w[53] = 0x0003;  // words 54-58 and 64-70 are valid; word 88 is not
    w[54] = g.cylinders;  // current CHS translation = the default one
    w[55] = g.heads;
    w[56] = g.sectors_per_track;
    w[57] = uint16_t(chs_sectors & 0xFFFF);
    w[58] = uint16_t(chs_sectors >> 16);
    w[60] = uint16_t(info.total_sectors & 0xFFFF);
    w[61] = uint16_t(info.total_sectors >> 16);
    w[63] = 0x0000;  // no multiword DMA
    w[64] = pio_mode >= 4 ? 0x0003 : pio_mode == 3 ? 0x0001 : 0x0000;  // advanced PIO modes 3, 4
    static const uint16_t CYCLE_NS[] = {600, 383, 240, 180, 120};  // ATA's t0, modes 0-4
    w[67] = CYCLE_NS[pio_mode];  // minimum PIO cycle time without IORDY, ns
    w[68] = CYCLE_NS[pio_mode];  // minimum PIO cycle time with IORDY, ns
    w[80] = 0x001E;  // supports ATA-1 to ATA-4
    w[82] = 1u << 5;               // write cache supported; no SMART or other sets
    w[83] = 0x4000 | (1u << 12);   // bit 14 shall be set; FLUSH CACHE; no 48-bit addressing
    w[84] = 0x4000;
    w[85] = 1u << 5;   // write cache enabled (SET FEATURES 02h/82h change it)
    w[86] = 1u << 12;  // FLUSH CACHE
    w[87] = 0x4000;
    fixChecksum(w);
}
