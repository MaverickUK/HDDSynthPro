// The microSD card (slice 5), on core 0. The drive's disk can be an image
// file on it: HDDSYNTH.IMG, or HDDBENCH.IMG for tests that write freely. The
// card may hold other files: nothing here ever formats it or touches any file
// but those two.
#pragma once

#include <Arduino.h>

#include "block_source.h"

namespace sd_card {

constexpr const char* IMAGE_NAME = "HDDSYNTH.IMG";
constexpr const char* BENCH_IMAGE_NAME = "HDDBENCH.IMG";  // scratch, for benchmarks

// Starts the card and prints one line: "SD ok type=... size_mb=... fs=..."
// or "SD error ...". Only reads. Blocks core 0 for up to about a second.
void printInfo(Print& out);

// Mounts the card and opens image `name` if it is there. Returns the image,
// or nullptr (no card, no image, or an error, written to `log`).
ImageFile* openImage(const char* name, Print& log);

// Creates image `name`, zero-filled and `sectors` long, if it doesn't exist
// and the card has room. Takes about a minute for 64 MB, during which core 0
// serves nothing else; the drive goes on using its current disk until reboot.
// Returns false (and says why) without changing anything if it can't.
bool createImage(const char* name, uint32_t sectors, Print& out);

}  // namespace sd_card
