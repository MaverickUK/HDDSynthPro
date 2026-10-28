// The drive's settings file on the SD card, HDDSYNTH.INI: which disk image
// to serve, and which transfer mode to offer. Plain text, one `key = value`
// per line, `#` starts a comment; any editor will do (Notepad's CRLF line
// endings and UTF-8 byte-order mark are fine).
//
//   image = HDDSYNTH.IMG   # the disk image, in the card's root folder; none: the built-in disk
//   mode = auto            # auto, or a transfer mode such as pio0
//
// A missing file, or a missing key, means the default. A mistake (an unknown
// key, a bad value, a key given twice) is reported and skipped: the drive
// always comes up.
//
// Pure C++, host-tested. sd_card.* reads and writes the file; main.cpp acts on it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace drive_config {

constexpr const char* FILE_NAME = "HDDSYNTH.INI";
constexpr const char* DEFAULT_IMAGE = "HDDSYNTH.IMG";
// `image = none`: no image, the built-in 64 MB test disk (writes kept in RAM
// until a restart), whose data the bus observer can check on the cable.
constexpr const char* NO_IMAGE = "none";
constexpr size_t MAX_FILE = 4096;      // longer files are read this far, with a warning
constexpr size_t MAX_NAME = 64;        // an image name, without the NUL
constexpr size_t MAX_WARNINGS = 4;
constexpr size_t MAX_WARNING = 96;

// The transfer modes ATA defines. Auto: the fastest the drive and the host
// both support. A specific mode: at most that one.
enum class Mode : uint8_t {
    Auto,
    Pio0, Pio1, Pio2, Pio3, Pio4,
    Mdma0, Mdma1, Mdma2,
    Udma0, Udma1, Udma2, Udma3, Udma4, Udma5, Udma6,
};

const char* modeName(Mode m);  // "auto", "pio0", ... "udma6"

// Can this firmware run `m`? Auto and PIO 0-3 so far.
bool modeSupported(Mode m);

// The PIO mode IDENTIFY offers for `m`: the mode itself, or AUTO_PIO_MODE for
// auto (and for a mode this firmware can't run, which falls back to auto).
uint8_t pioModeOffered(Mode m);

// What `auto` offers: the fastest mode there is. The adapter asks for mode 4,
// then takes the fastest IDENTIFY offers from word 64 (3), never 1 or 2.
constexpr uint8_t AUTO_PIO_MODE = 3;

struct Config {
    char image[MAX_NAME + 1];
    Mode mode = Mode::Auto;
    char warnings[MAX_WARNINGS][MAX_WARNING];
    size_t warning_count = 0;  // may exceed MAX_WARNINGS: only the first ones are kept
};

// The settings in `text` (`len` bytes, not necessarily NUL-terminated), on
// top of the defaults. Mistakes go into `warnings`.
void parse(const char* text, size_t len, Config& out);

// The defaults, as parse("") gives them.
void defaults(Config& out);

// False for `image = none` (any case): serve the built-in disk, not an image.
bool servesImage(const Config& c);

// `text` with `key` set to `value`, into `out` (NUL-terminated): the line
// setting it is replaced in place, keeping its comment, or a new line is
// added at the end. Comments, other lines and the line endings in use are
// kept. Returns the new length, or 0 if `out` is too small or `key` is not
// a known key.
size_t setKey(const char* text, size_t len, const char* key, const char* value, char* out, size_t size);

// The file a `C` console command creates: the defaults, commented.
const char* defaultFile();

}  // namespace drive_config
