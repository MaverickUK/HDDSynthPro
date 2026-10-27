#pragma once

// SemVer. Bump in the same commit as any firmware change (see CLAUDE.md).
#define FIRMWARE_VERSION "0.9.0"

// What this build does, set per environment in platformio.ini:
//   0  listen only (slice 0): every pin an input, raw bus trace
//   3  the drive (slices 1-5): answers the bus, IDENTIFY, READ and WRITE
//      SECTORS on the generated disk or an SD image, and the housekeeping
//      commands; traces commands
#ifndef FIRMWARE_SLICE
#define FIRMWARE_SLICE 0
#endif
