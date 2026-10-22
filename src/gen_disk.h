// The slice 3 test disk: every sector's content is computed from its LBA,
// so the disk needs no storage and can be any size. Pure C++.
//
// host/hddproto/gen_disk.py is the Python twin used by the rig tests; both
// are checked against the same known answers, so change them together.
//
// Layout of sector `lba` (512 bytes):
//   bytes 0-3   lba, little-endian      (a misplaced sector names itself)
//   bytes 4-7   ~lba, little-endian
//   bytes 8-511 a byte stream from a 32-bit LCG seeded from lba: neighbouring
//               sectors and byte offsets differ, so swaps and shifts show up
#pragma once

#include <cstdint>

namespace gen_disk {

constexpr uint32_t SECTOR_BYTES = 512;
constexpr uint32_t DEFAULT_SECTORS = 131072;  // 64 MB

inline void sector(uint32_t lba, uint8_t out[SECTOR_BYTES]) {
    uint32_t inv = ~lba;
    for (int i = 0; i < 4; ++i) {
        out[i] = uint8_t(lba >> (8 * i));
        out[4 + i] = uint8_t(inv >> (8 * i));
    }
    uint32_t state = lba * 2654435761u + 0x12345678u;
    for (uint32_t i = 8; i < SECTOR_BYTES; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = uint8_t(state >> 24);
    }
}

}  // namespace gen_disk
