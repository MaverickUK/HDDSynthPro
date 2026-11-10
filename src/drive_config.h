// The drive's settings file on the SD card, HDDSYNTH.INI: which disk image
// to serve, and which transfer mode to offer. Plain text, one `key = value`
// per line, `#` starts a comment; any editor will do (Notepad's CRLF line
// endings and UTF-8 byte-order mark are fine).
//
//   device0 = HDDSYNTH.IMG # device 0's disk image, in the card's root folder; none: the built-in disk;
//                          # cdrom (or an .ISO/.CUE): a CD-ROM drive alone (`image`, its old name, still works)
//   device1 = GAMES        # a second drive, device 1: an image or a folder (read-only), or cdrom
//                          # (or an .ISO/.CUE) for a CD-ROM drive; none by default
//   mode = auto           # auto, or a transfer mode such as pio0
//   dma = off              # whether auto may offer multiword DMA (where DMARQ is wired)
//   write_behind = on      # acknowledge a write while it's stored (faster)
//   read_ahead = on        # fetch the next sector while the host reads this one
//   activity_link = on     # tell an HDD Synth what the drive is doing
//   seek_timing = off      # hold commands for as long as a modelled drive would
//   isa = off              # a 16-bit ISA IDE card: IORDY let go if held for 23 us
//   early_answers = on     # answers ready at the chip select, for hosts that don't wait for IORDY
//   iordy = on             # off: never touch IORDY, for hosts that wait for it before the strobe
//   geometry = auto        # from the image's partition table, else 16 x 63; or C/H/S, e.g. 756/13/51
//   cdrom = HDDSYNTH.ISO   # the CD-ROM drive's disc (device0 or device1 = cdrom): an ISO or a CUE
//                          # sheet; none: the test disc
//
// A missing file, or a missing key, means the default. A mistake (an unknown
// key, a bad value, a key given twice) is reported and skipped: the drive
// always comes up.
//
// Pure C++, host-tested. sd_card.* reads and writes the file; main.cpp acts on it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "identify.h"
#include "seek_timing.h"

namespace drive_config {

constexpr const char* FILE_NAME = "HDDSYNTH.INI";
constexpr const char* DEFAULT_IMAGE = "HDDSYNTH.IMG";
// `device0 = none`: no image, the built-in 64 MB test disk (writes kept in RAM
// until a restart), whose data the bus observer can check on the cable.
constexpr const char* NO_IMAGE = "none";
// The CD-ROM drive's disc (docs/atapi.md), for `device0 = cdrom` or `device1 = cdrom`: an ISO or a
// CUE sheet (with its BIN) in the root folder, or `cdrom = none` for the generated test disc. No
// such file: no disc in the drive.
constexpr const char* DEFAULT_CDROM = "HDDSYNTH.ISO";
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

// Can this firmware run `m`? Auto, PIO 0-3 and multiword DMA 0-2 so far.
bool modeSupported(Mode m);

// The PIO mode IDENTIFY offers for `m`: the mode itself, or AUTO_PIO_MODE for
// auto (and for a mode this firmware can't run, which falls back to auto).
uint8_t pioModeOffered(Mode m);

// The multiword DMA mode IDENTIFY offers for `m`, or NO_DMA (identify.h).
// Never without DMARQ wired (`dmarq_wired`, tested at start-up): a host told
// the drive does DMA would wait for DMARQ forever. mdma0-2 offer that mode;
// auto offers the fastest only when `auto_dma` (the `dma` setting) allows it.
uint8_t mdmaModeOffered(Mode m, bool dmarq_wired, bool auto_dma);

// True if `m` asks for DMA that mdmaModeOffered() won't give, for want of
// DMARQ: worth a warning.
bool dmaRefusedForWiring(Mode m, bool dmarq_wired);

// What `auto` offers: the fastest mode there is. The adapter asks for mode 4,
// then takes the fastest IDENTIFY offers from word 64 (3), never 1 or 2.
constexpr uint8_t AUTO_PIO_MODE = 3;

// The on/off settings' defaults. DMA stays off for `auto` until it has had a
// long test (the DMA soak); `mode = mdma2` asks for it meanwhile.
constexpr bool DEFAULT_DMA = false;
constexpr bool DEFAULT_WRITE_BEHIND = true;
constexpr bool DEFAULT_READ_AHEAD = true;
constexpr bool DEFAULT_ACTIVITY_LINK = true;
// `isa = on`: release IORDY when it has been held for about 23 us
// (iordy_watchdog.pio). For ISA IDE cards that pass IORDY to the ISA bus's
// IOCHRDY and select the drive from the address alone, so a memory cycle
// whose address aliases 1F0h-1F7h would otherwise stall for good (an ICL 386,
// 0.21.0). Off by default: ATA hosts never need it.
constexpr bool DEFAULT_ISA = false;
// `early_answers = on`: ide_bus_early, which asks core 1 for a register's
// value at the chip select, so hosts that don't wait for IORDY get it in time
// (docs/no-iordy.md: a Pentium's PIIX during the BIOS's detection). On by
// default since 0.27.0: it still holds IORDY for hosts that wait for it, and
// core 1 lets IORDY go after cfg::IORDY_STALL_US with no strobe, which keeps
// ISA cards safe too. `isa = on` overrides it with the older program.
constexpr bool DEFAULT_EARLY_ANSWERS = true;
// `iordy = off`: the drive never touches IORDY (the pin stays an input). For
// hosts that check IORDY before they start a strobe, which the drive's IORDY
// held from the chip select deadlocks (an Amiga 1200's Gayle, 2026-10-31);
// with early_answers the value is ready by the strobe anyway.
constexpr bool DEFAULT_IORDY = true;

struct Config {
    // `device0 = NAME`: device 0's disk image. Its old name, `image`, still
    // works; if a file has both, device0 counts.
    char device0[MAX_NAME + 1];
    // `device1 = NAME`: a second drive, device 1 (docs/two-drives.md), an image
    // file or a folder in the card's root folder (served read-only); empty (or
    // `none`): no device 1, as before.
    char device1[MAX_NAME + 1];
    char cdrom[MAX_NAME + 1];  // the CD-ROM drive's disc, for `device0` or `device1 = cdrom`
    Mode mode = Mode::Auto;
    bool dma = DEFAULT_DMA;                    // `auto` may offer multiword DMA
    bool write_behind = DEFAULT_WRITE_BEHIND;  // see Device::setWriteBehind
    bool read_ahead = DEFAULT_READ_AHEAD;      // see Device::setReadAhead
    bool activity_link = DEFAULT_ACTIVITY_LINK;
    bool isa = DEFAULT_ISA;
    bool early_answers = DEFAULT_EARLY_ANSWERS;
    bool iordy = DEFAULT_IORDY;
    // `geometry = C/H/S`: the default geometry IDENTIFY reports, for an image
    // from an old drive whose partition table and boot code use its own (a
    // BIOS's auto-detect then picks it up); 0 cylinders: auto (16 x 63).
    Geometry geometry{};
    ::seek_timing::Setting seek_timing;  // off by default
    char warnings[MAX_WARNINGS][MAX_WARNING];
    size_t warning_count = 0;  // may exceed MAX_WARNINGS: only the first ones are kept
};

// The settings in `text` (`len` bytes, not necessarily NUL-terminated), on
// top of the defaults. Mistakes go into `warnings`.
void parse(const char* text, size_t len, Config& out);

// The defaults, as parse("") gives them.
void defaults(Config& out);

// False for `device0 = none` (any case): serve the built-in disk, not an image.
bool servesImage(const Config& c);
// True if `device1` names a disk for device 1.
bool hasDevice1(const Config& c);
// False for `cdrom = none`: the CD-ROM drive serves the test disc.
bool servesDisc(const Config& c);
// True if device 0 is to be a CD-ROM drive, alone on the cable: `device0 =
// cdrom` (the `cdrom` setting's disc), or a device0 name ending in .ISO or
// .CUE. Device 1 is then left out, whatever `device1` says.
bool device0IsCdrom(const Config& c);
// That drive's disc: the ISO device0 names, or `cdrom`'s (which may be `none`).
const char* device0Disc(const Config& c);
// True if device 1 is to be a CD-ROM drive beside device 0's disk: `device1 =
// cdrom` (the `cdrom` setting's disc), or a device1 name ending in .ISO or .CUE.
bool device1IsCdrom(const Config& c);
// That drive's disc: the ISO device1 names, or `cdrom`'s (which may be `none`).
const char* device1Disc(const Config& c);

// `text` with `key` set to `value`, into `out` (NUL-terminated): the line
// setting it is replaced in place, keeping its comment, or a new line is
// added at the end. Comments, other lines and the line endings in use are
// kept. An old name counts as its key (`image` as `device0`), either way
// round, and a line using it is rewritten with the new name. Returns the
// new length, or 0 if `out` is too small or `key` is not a known key (isKey).
size_t setKey(const char* text, size_t len, const char* key, const char* value, char* out, size_t size);

// The file a `C` console command creates: the defaults, commented.
const char* defaultFile();

// True for a key the file can set (case doesn't matter).
bool isKey(const char* key);

// "on" or "off", for printing a setting.
inline const char* onOff(bool on) { return on ? "on" : "off"; }

}  // namespace drive_config
