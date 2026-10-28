// The microSD card (slice 5), on core 0. The drive's disk can be an image
// file on it: HDDSYNTH.IMG, the one HDDSYNTH.INI names, or HDDBENCH.IMG for
// tests that write freely. The card may hold other files: nothing here ever
// formats it, and the only files it writes are the image being served,
// images it is asked to create, and HDDSYNTH.INI.
#pragma once

#include <Arduino.h>

#include "block_source.h"
#include "media_watch.h"

namespace sd_card {

constexpr const char* IMAGE_NAME = "HDDSYNTH.IMG";
constexpr const char* BENCH_IMAGE_NAME = "HDDBENCH.IMG";  // scratch, for benchmarks

// Starts the card and prints one line: "SD ok type=... size_mb=... fs=..."
// or "SD error ...". Only reads. Blocks core 0 for up to about a second.
void printInfo(Print& out);

// Mounts the card and opens image `name` if it is there. Returns the image,
// or nullptr (no card, no image, or an error, written to `log`).
// Closes any image opened before. `name` must outlive the open image.
ImageFile* openImage(const char* name, Print& log);
// Why the last openImage() returned nullptr: "no card", "not found" or "can't open".
const char* openError();

// The settings file (drive_config.h). readConfig() reads up to `size` bytes
// of it into `buf`: false if there is no card or no file; `truncated` if the
// file was longer. writeConfig() replaces it (or creates it) with `text`.
// readConfig() mounts the card first if it isn't (errors go to `log`).
bool readConfig(char* buf, size_t size, size_t& len, bool& truncated, Print& log);
bool writeConfig(const char* text, size_t len, Print& out);
bool configExists();
// Whether the card is mounted: after a failed mount at boot, the caller
// skips the images rather than waiting for the card again for each one.
bool mounted();

// Creates image `name`, zero-filled and `sectors` long, if it doesn't exist
// and the card has room. Takes about a minute for 64 MB, during which core 0
// serves nothing else; the drive goes on using its current disk until reboot.
// Returns false (and says why) without changing anything if it can't.
bool createImage(const char* name, uint32_t sectors, Print& out);

// The card the image is on (media_watch.h). A card pulled out is noticed when
// an image read or write fails; from then on they fail at once, and the image
// is served again only when the same card comes back with it unchanged.
// A read or write that fails is retried once after restarting the card, so a
// card pulled and put back while the drive was idle costs nothing.
const media_watch::Watch& media();
// Core 0's loop: while the card is lost, looks for it coming back (a CMD0
// every media_watch::PROBE_MS, under a millisecond); while a different card
// is in, checks that it still is. Restarting a card that answers takes tens
// of milliseconds.
void serviceMedia();

}  // namespace sd_card
