#pragma once

// SemVer. Bump in the same commit as any firmware change (see CLAUDE.md).
#define FIRMWARE_VERSION "0.6.2"

// What this build does, set per environment in platformio.ini:
//   0  listen only (slice 0): every pin an input, raw bus trace
//   3  the drive (slices 1-3): answers the bus, IDENTIFY, READ SECTORS from
//      the generated disk, and the housekeeping commands; traces commands
#ifndef FIRMWARE_SLICE
#define FIRMWARE_SLICE 0
#endif
