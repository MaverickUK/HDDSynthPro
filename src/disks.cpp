// Core 0: which disks the drive serves. The choice at boot (HDDSYNTH.INI, the
// card's images, a folder, a CD-ROM's disc, the welcome and test disks), the
// CD-ROM's disc changed at run time (console `M`), and the settings: console
// `E` and `S`. Moved out of main.cpp (0.33.1-dev.7); main_state.h declares
// what the files share. Nothing here runs on core 1.
#include <Arduino.h>

#include <hardware/gpio.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "bus_sample.h"
#include "config.h"
#include "core1_ram.h"
#include "drive_device.h"
#include "short_test.h"
#include "stack_watch.h"
#include "spsc_ring.h"
#include "trace_format.h"
#include "version.h"

#if FIRMWARE_SLICE == 0
#include "ide_listen.h"
#else
#include <hardware/clocks.h>
#include <hardware/structs/busctrl.h>
#include <hardware/uart.h>

#include "access_log.h"
#include "activity.h"
#include "auto_bus.h"
#include "activity_link.h"
#include "drive_config.h"
#include "drive_params.h"
#include "drive_timing.h"
#include "identify.h"
#include "seek_timing.h"
#include "sentence.h"
#include "ata_device.h"
#include "hdcd_device.h"
#include <new>
#include <type_traits>
#include "block_source.h"
#include "cue_sheet.h"
#include "bus_observer.h"
#include <hardware/watchdog.h>
#include "fetch_mailbox.h"
#include "ide_bus.h"
#include "ide_table.h"
#include "ide_dma.h"
#include "iocs16.h"
#include "bus_timing.h"
#include "command_history.h"
#include "pio_plan.h"
#include "mmc.h"
#include <hardware/structs/m33.h>
#include "iordy_timer.h"
#include "sd_card.h"
#include "scratch.h"
#include "synthminder.h"
#include "welcome_disk.h"
#include "welcome_text.h"
#endif

#include "heap_limit.h"
#include "main_state.h"

#if FIRMWARE_SLICE != 0

// Core 0's share of the drive: move the sectors core 1 asks for. The disk is
// an image on the SD card (slice 5) if there is one to serve (chooseDisk()),
// else the welcome disk, or the generated test disk with `device0 = none` or
// console `T`. Both keep writes in the one RAM overlay, until a restart.
static const welcome_disk::File WELCOME_FILES[] = {
    {"README.TXT", welcome_text::README, welcome_text::README_SIZE},
    {drive_config::FILE_NAME, drive_config::defaultFile(), uint32_t(strlen(drive_config::defaultFile()))},
};
static WelcomeDisk g_welcome(WELCOME_FILES, 2, welcome_text::FAT_DATE);
static GeneratedDisk g_generated;
static RamOverlayDisk<cfg::RAM_DISK_SECTORS> g_overlay(g_welcome);
BlockSource* g_disk = &g_overlay;
const char* g_disk_name = "welcome";
bool g_test_disk = false;  // the generated disk: the bus observer can check its data

// Device 1's disk (`device1` in HDDSYNTH.INI, docs/two-drives.md), or nullptr:
// a second image, or a folder served read-only.
BlockSource* g_device1_disk = nullptr;
const char* g_device1_kind = "none";  // "none", "image" or "folder"
static char g_device1_note[112] = "";         // why the device1 configured isn't served
static folder_disk::Volume g_folder;
static FolderDisk g_folder_disk(g_folder);
static uint16_t g_folder_capacity = 0;  // entries the folder's list has room for

// A CD-ROM drive as device 0 with no disc to serve: a drive with no disc in it.
class EmptyDrive : public BlockSource {
public:
    uint32_t sectors() const override { return 0; }
    bool read(uint32_t, uint16_t[256]) override { return false; }
};
static EmptyDrive g_no_disc;

uint32_t g_disk_ms = 0;  // how long choosing the disk took at boot (console `s`)

// Console `J`: serve HDDBENCH.IMG after the restart it triggers. Kept in a
// watchdog scratch register, which a restart keeps and a power cycle clears,
// so the drive is always back on HDDSYNTH.IMG after power-off.
bool benchRequested() { return watchdog_hw->scratch[0] == BENCH_MAGIC; }
// Console `T`: serve the test disk after the restart it triggers, card or no
// card, in the same register (so `T` and `J` undo each other).
bool testDiskRequested() { return watchdog_hw->scratch[0] == TEST_DISK_MAGIC; }
// Console `P`: a bus program after the restart it triggers, whatever
// HDDSYNTH.INI says, until a power cycle (watchdog scratch 1): `e` early
// answers, `l` the data lane, `i` ide_bus (as `isa = on`), `t` the register
// table, `a` auto (as `bus = auto`), `0` the settings'.
// For the rig, to compare programs with no card or without editing the file.
static char busRequested() {
    uint32_t v = watchdog_hw->scratch[1];
    char c = char(v & 0xff);
    return (v & 0xffffff00) == BUS_MAGIC && (c == 'e' || c == 'l' || c == 'i' || c == 't' || c == 'a') ? c : 0;
}

// The generated test disk instead of the welcome disk, under the RAM overlay.
static void useTestDisk() {
    g_overlay.setBase(g_generated);
    g_disk_name = "generated";
    g_test_disk = true;
}

// HDDSYNTH.INI (drive_config.h) as read at boot, and what came of it. Boot
// messages go out before USB serial is connected, so console `S` shows them.
drive_config::Config g_config;
const char* g_config_state = "none";  // "none", "present" or "truncated"
const char* g_image_name = "-";        // the image being served
static char g_image_note[96] = "";            // why the configured image isn't

// Opens image `name` as the disk. False, with sd_card::openError() or
// "empty" as the reason in `why`, if it can't be used.
[[maybe_unused]] static bool useImage(const char* name, const char*& why, bool read_only = false) {
    ImageFile* file = sd_card::openImage(name, Serial, read_only);
    if (!file) {
        why = sd_card::openError();
        return false;
    }
    static ImageDisk image(*file);  // always the same file object, reopened
    if (image.sectors() == 0) {
        why = "empty";
        return false;
    }
    g_disk = &image;
    g_image_name = name;
    return true;
}

// A CD-ROM's disc from the card, read-only: an ISO as it is, or a CUE
// sheet's data track from its BIN (cue_sheet.h). In device 0's image slot for
// `device0 = cdrom`, device 1's for `device1 = cdrom` (never both: a CD-ROM
// as device 0 has no device 1). nullptr if it can't be used, with why.
static BlockSource* openCdDisc(const char* name, const char*& why, uint8_t unit) {
    auto open = unit ? sd_card::openDevice1Image : sd_card::openImage;
    if (!cue_sheet::isCueName(name)) {
        ImageFile* file = open(name, Serial, true);
        if (!file) {
            why = sd_card::openError();
            return nullptr;
        }
        static ImageDisk iso(*file);  // always the same file object, reopened
        if (!iso.sectors()) why = "empty";
        return iso.sectors() ? &iso : nullptr;
    }
    static_assert(cue_sheet::MAX_TEXT <= scratch::SIZE);
    char* const text = scratch::b;  // only until this returns: the sheet's fields are copied out
    size_t len = 0;
    bool truncated = false;
    if (!sd_card::readFileStart(name, text, cue_sheet::MAX_TEXT, len, truncated, why, Serial)) return nullptr;
    if (truncated)  // a long sheet: up to its last whole line
        while (len && text[len - 1] != '\n') --len;
    cue_sheet::Sheet parsed;
    if (!cue_sheet::parse(text, len, parsed)) {
        why = parsed.error;
        return nullptr;
    }
    // Kept only now: the card code holds on to the open BIN's name (to reopen
    // it after the card comes back), and a sheet that failed mustn't blank it.
    static cue_sheet::Sheet sheet;
    sheet = parsed;
    ImageFile* file = open(sheet.file, Serial, true);
    if (!file) {
        static char bin_why[cue_sheet::MAX_FILE_NAME + 24];
        std::snprintf(bin_why, sizeof bin_why, "%s %s", sheet.file, sd_card::openError());
        why = bin_why;
        return nullptr;
    }
    static RawTrackDisk raw(*file);
    raw.setTrack(sheet.track);
    if (!raw.sectors()) {
        why = "its data track is empty";
        return nullptr;
    }
    Serial.printf("# %s: %s, track 1 of %u%s (%lu-byte frames from byte %lu), %lu frames\n", name, sheet.file,
                  unsigned(sheet.tracks), truncated ? "+" : "", (unsigned long)sheet.track.stride, (unsigned long)sheet.track.data_offset,
                  (unsigned long)(raw.sectors() / atapi::SECTORS_PER_FRAME));
    return &raw;
}

// The geometry an image's partition table was made with, packed as
// g_geometry is, or 0 (then 16 x 63).
static uint32_t partitionGeometry(BlockSource& disk) {
    static uint16_t sector0[256];  // bus order: as bytes, as on disk
    if (!disk.read(0, sector0)) return 0;
    Geometry geo = geometryFromPartitions(reinterpret_cast<const uint8_t*>(sector0), disk.sectors());
    if (!geo.cylinders) return 0;
    return uint32_t(geo.cylinders) << 16 | uint32_t(geo.heads) << 8 | geo.sectors_per_track;
}

// A CD-ROM drive as device 1 (`device1 = cdrom`, or an ISO): its disc, as
// chooseCdrom0 chooses device 0's. The generated test disc after console `T`
// or with `cdrom = none` (read-only, so the disk's RAM overlay isn't shared);
// else the ISO; else a drive with no disc in it.
static GeneratedDisk g_test_disc;
static const char* g_device1_disc = "no disc";

static void chooseCdrom1() {
    g_device1_cdrom = true;
    g_device1_kind = "cdrom";
    const char* name = drive_config::device1Disc(g_config);
    if (testDiskRequested() || (sd_card::mounted() && !drive_config::servesDisc(g_config))) {
        g_device1_disk = &g_test_disc;
        g_device1_disc = "test disc";
    } else if (!sd_card::mounted()) {
        std::snprintf(g_device1_note, sizeof g_device1_note, "%s: no card", name);
    } else {
        const char* why = "";
        if ((g_device1_disk = openCdDisc(name, why, 1)))
            g_device1_disc = name;
        else
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: %s", name, why);
    }
    g_device1_sectors.store(g_device1_disk ? g_device1_disk->sectors() : 0, std::memory_order_relaxed);
}

// A CD-ROM drive alone as device 0 (`device0 = cdrom`, or an ISO), as the
// CD-ROM firmware (bb48_cdrom, up to 0.31.0) served it: the generated test
// disc after console `T` or with `cdrom = none`; else the disc named; else
// a drive with no disc in it. With no card: no disc.
static void chooseCdrom0() {
    g_device0_cdrom = true;
    g_disk = &g_no_disc;
    g_disk_name = "no disc";
    const char* name = drive_config::device0Disc(g_config);
    const char* why = "";
    if (testDiskRequested() || (sd_card::mounted() && !drive_config::servesDisc(g_config))) {
        g_disk = &g_overlay;  // read-only here, so the overlay stays empty
        useTestDisk();
    } else if (!sd_card::mounted()) {
        std::snprintf(g_image_note, sizeof g_image_note, "%s: no card", name);
    } else if (BlockSource* disc = openCdDisc(name, why, 0)) {  // a disc: read-only
        g_disk = disc;
        g_image_name = name;
        g_disk_name = "disc";
    } else {
        std::snprintf(g_image_note, sizeof g_image_note, "%s: %s", name, why);
    }
    if (drive_config::hasDevice1(g_config))
        std::snprintf(g_device1_note, sizeof g_device1_note, "%s: device 0 is a CD-ROM drive, alone",
                      g_config.device1);
}

// Device 1's disk, from `device1` (docs/two-drives.md): a folder, laid out as
// a read-only FAT16 volume, or an image. Not with the test or bench disk
// asked for from the console, which are for the rig's tests.
static void chooseDevice1() {
    if (drive_config::device1IsCdrom(g_config)) {
        chooseCdrom1();
        return;
    }
    if (!drive_config::hasDevice1(g_config) || !sd_card::mounted() || testDiskRequested() || benchRequested())
        return;
    const char* name = g_config.device1;
    if (sd_card::isFolder(name)) {
        // The list on the heap, only when a folder is served: as long as RAM
        // allows, leaving cfg::FOLDER_HEAP_LEFT for whatever allocates later
        // (heapFree(): rp2040.getFreeHeap() counts the DMA rings as free).
        // malloc, not new (std::nothrow): Arduino-Pico's libstdc++ is built
        // without exceptions, so a nothrow new that runs out aborts (the board
        // never started with a folder, 3 Nov 2026). An Entry needs no constructor.
        uint16_t capacity = cfg::FOLDER_MAX_ENTRIES;
        folder_disk::Entry* entries = nullptr;
        while (capacity >= 64) {
            entries = static_cast<folder_disk::Entry*>(std::malloc(capacity * sizeof(folder_disk::Entry)));
            if (entries && heapFree() >= cfg::FOLDER_HEAP_LEFT) break;
            std::free(entries);
            entries = nullptr;
            capacity /= 2;
        }
        if (!entries) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: no RAM for its list", name);
            return;
        }
        if (!g_folder.build(sd_card::folderSource(name), entries, capacity, name)) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: can't list it, or nothing fits", name);
            std::free(entries);
            return;
        }
        g_folder_capacity = capacity;
        g_device1_disk = &g_folder_disk;
        g_device1_kind = "folder";
    } else if (strcasecmp(name, g_image_name) == 0) {
        // The image device 0 serves (named, or fallen back to): the PC would see
        // one filesystem as two disks, and writes through either would corrupt it.
        std::snprintf(g_device1_note, sizeof g_device1_note, "%s: device 0 serves it already", name);
        return;
    } else {
        ImageFile* file = sd_card::openDevice1Image(name, Serial);
        if (!file) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: %s", name, sd_card::openError());
            return;
        }
        static ImageDisk image(*file);
        if (image.sectors() == 0) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: empty", name);
            return;
        }
        g_device1_disk = &image;
        g_device1_kind = "image";
        g_device1_geometry.store(partitionGeometry(image), std::memory_order_relaxed);
    }
    g_device1_sectors.store(g_device1_disk->sectors(), std::memory_order_relaxed);
}

// The disk: the test disk after console `T`, or HDDBENCH.IMG after `J`; else
// the image HDDSYNTH.INI names (HDDSYNTH.IMG by default), or the test disk
// for `device0 = none`; else HDDSYNTH.IMG; else the welcome disk.
// The geometry setting, for the image only (the welcome and generated disks
// lay theirs out for the default), its cylinders cut to fit if it's bigger.
// At start, and for a new disk at the host's reset (serviceResetSwap).
static void chooseGeometry() {
    const bool image = strcmp(g_disk_name, "image") == 0;
    Geometry geo = image ? g_config.geometry : Geometry{};
    g_geometry_from = geo.cylinders ? "set" : "auto";
    if (image && !geo.cylinders) {  // auto: the geometry the image's partition table was made with
        static uint16_t sector0[256];  // bus order: as bytes, as on disk
        if (g_disk->read(0, sector0)) {
            geo = geometryFromPartitions(reinterpret_cast<const uint8_t*>(sector0), g_disk->sectors());
            if (geo.cylinders) g_geometry_from = "partition table";
        }
    }
    g_geometry_trimmed = false;
    if (geo.cylinders && geo.sectors() > g_disk->sectors()) {
        geo.cylinders = uint16_t(g_disk->sectors() / (uint32_t(geo.heads) * geo.sectors_per_track));
        g_geometry_trimmed = true;
    }
    g_geometry.store(geo.cylinders ? uint32_t(geo.cylinders) << 16 | uint32_t(geo.heads) << 8 | geo.sectors_per_track : 0,
                     std::memory_order_relaxed);
}

void chooseDisk() {
    uint32_t start = millis();
    drive_config::defaults(g_config);
    static_assert(drive_config::MAX_FILE <= scratch::SIZE);
    char* const text = scratch::a;  // only for the parse: g_config holds copies
    size_t len = 0;
    bool truncated = false;
    if (sd_card::readConfig(text, drive_config::MAX_FILE, len, truncated, Serial)) {
        drive_config::parse(text, len, g_config);
        g_config_state = truncated ? "truncated" : "present";
    }
    sd_card::setHighSpeed(g_config.sd_high_speed);  // before the image opens, so it's read at the new speed
    const char* why = "";
    if (drive_config::device0IsCdrom(g_config) && !benchRequested()) {
        chooseCdrom0();  // console `J` still serves the bench image, as a hard disk
    } else if (testDiskRequested()) {
        useTestDisk();  // the card, if any, left alone
    } else if (!sd_card::mounted()) {
        // no card: the welcome disk, without waiting for the card again
    } else if (benchRequested()) {
        if (useImage(sd_card::BENCH_IMAGE_NAME, why)) g_disk_name = "bench";
    } else if (!drive_config::servesImage(g_config)) {
        useTestDisk();  // device0 = none: the card left alone
    } else if (useImage(g_config.device0, why)) {
        g_disk_name = "image";
    } else {
        std::snprintf(g_image_note, sizeof g_image_note, "%s: %s", g_config.device0, why);
        if (strcasecmp(g_config.device0, drive_config::DEFAULT_IMAGE) != 0 &&
            useImage(drive_config::DEFAULT_IMAGE, why))
            g_disk_name = "image";
    }
    g_disk_ms = millis() - start;
    g_pio_mode.store(drive_config::pioModeOffered(g_config.mode), std::memory_order_relaxed);
    g_mode_asked.store(uint8_t(g_config.mode), std::memory_order_relaxed);  // core 1 decides DMA from it
    // The on/off settings (the console's W, A and L change them until a restart).
    g_dma_auto.store(g_config.dma);
    g_dma_when_learned.store(g_config.dma_auto);
    g_write_behind.store(g_config.write_behind);
    g_read_ahead.store(g_config.read_ahead);
    g_link_on = g_config.activity_link;
    g_isa.store(g_config.isa);  // core 1 sets up the bus program from it
    // isa = on keeps its own program (no early answers, iordy_watchdog.pio):
    // the one the ICL 386 was proven on
    g_early.store(g_config.early_answers && !g_config.isa);
    // register_table = on: the table in place of early answers or the data
    // lane; isa = on keeps its own program
    g_table.store(g_config.register_table && !g_config.isa);
    // bus (docs/auto-bus.md), over early_answers and register_table; isa = on
    // keeps its own program, and iordy = off its own (bus = auto never switches it).
    g_bus_auto.store(false);
    if (!g_config.isa) {
        switch (g_config.bus) {
            case drive_config::Bus::Auto:
                g_early.store(true);  // every power-up and reset starts on early answers
                g_table.store(false);
                g_bus_auto.store(g_config.iordy);
                break;
            case drive_config::Bus::Early: g_early.store(true); g_table.store(false); break;
            case drive_config::Bus::Lane: g_early.store(false); g_table.store(false); break;
            case drive_config::Bus::IdeBus: g_early.store(false); g_table.store(false); g_no_lane.store(true); break;
            default: break;
        }
    }
    if (busRequested()) g_bus_auto.store(false);
    switch (busRequested()) {  // console `P`, over the settings
        case 'a': g_isa.store(false); g_early.store(true); g_table.store(false); g_bus_auto.store(g_config.iordy); break;
        case 'e': g_isa.store(false); g_early.store(true); g_table.store(false); break;
        case 't': g_isa.store(false); g_early.store(false); g_table.store(true); break;
        case 'l': g_isa.store(false); g_early.store(false); g_table.store(false); break;
        case 'i': g_isa.store(true); g_early.store(false); g_table.store(false); break;
        default: break;
    }
    g_iordy.store(g_config.iordy);
    chooseGeometry();
    // A host that ignores IORDY reads whatever is on the bus: check image data
    // on the cable too, with early answers and on the data lane. Not with
    // `isa = on`: the ISA card that needs it strobes DIOR# with no chip select,
    // which the observer takes for data.
    bus_observer::setImageChecks(!g_isa.load() && !g_test_disk);
    if (!g_device0_cdrom) chooseDevice1();  // a CD-ROM as device 0 is alone on the cable
    // Core 1 shares out the PIO blocks next, by whether the card holds one: so
    // after device 1's image, folder or disc is open (with device 0 on the
    // test disk the card would otherwise be parked under them).
    sd_card::sharePio();
    // A CD-ROM, as device 0 or 1, runs on ide_bus (as console `P i`), never the
    // data lane (written for the hard disk's 256-word blocks). On early answers
    // hdcd::Device answers too slowly (Status held 1.8-2.4 us): the USB
    // adapter's register check after RESET# never passed, so it never detected
    // the drive and dropped off USB (device 1: 4 Nov, 0.30.0-dev.10; device 0:
    // 9 Nov, 0.32.0-dev.2, where bb48_cdrom's own atapi::Device had managed).
    // Console `P e` still picks early answers, to reproduce it.
    // `isa` stays as set: forcing it on here (up to 0.32.1-dev.4) also started
    // iordy_watchdog on PCI hosts, which released IORDY mid-read on the
    // Evesham about once in 2.5 million data words, so the host latched DD
    // before the drive drove it (docs/atapi.md).
    if (g_device0_cdrom || g_device1_cdrom) g_table.store(false);  // the register table is the hard disk's, so far
    if ((g_device0_cdrom || g_device1_cdrom) && busRequested() != 'e') {
        g_no_lane.store(true);
        g_early.store(false);
        g_bus_auto.store(false);  // a CD-ROM runs ide_bus: nothing to switch
    }
    g_disk_sectors.store(g_disk->sectors(), std::memory_order_relaxed);
    g_disk_chosen.store(true, std::memory_order_release);  // after the mode and device 1: core 1 reads them
}

// Console `M`: a disc change, step by step from core 0's loop (serviceDiscChange).
enum class DiscStep : uint8_t { None, Ejecting, Inserting };
static DiscStep g_disc_step = DiscStep::None;
static char g_disc_next[drive_config::MAX_NAME + 1];

static void requestDisc(uint32_t sectors) {
    g_disc_sectors.store(sectors, std::memory_order_relaxed);
    g_disc_seq.fetch_add(1, std::memory_order_release);
}

// The disc named, as the CD-ROM's disk: `none` (no disc), `test` (the
// generated test disc), or an ISO on the card, read-only. Returns its sectors,
// 0 for none (or an ISO that can't be used, which it says why).
static uint32_t openDisc(const char* name) {
    const bool none = strcasecmp(name, "none") == 0, test = strcasecmp(name, "test") == 0;
    if (g_device0_cdrom) {
        g_test_disk = false;
        g_disk = &g_no_disc;
        g_disk_name = "no disc";
        const char* why = "";
        if (test) {
            g_disk = &g_overlay;  // read-only here, so the overlay stays empty
            useTestDisk();
        } else if (BlockSource* disc = none ? nullptr : openCdDisc(name, why, 0)) {
            g_disk = disc;
            g_image_name = g_disc_next;
            g_disk_name = "disc";
        } else if (!none) {
            Serial.printf("# DISC %s: %s\n", name, why);
        }
        return g_disk->sectors();
    }
    g_device1_disk = nullptr;
    g_device1_disc = "no disc";
    if (test) {
        g_device1_disk = &g_test_disc;
        g_device1_disc = "test disc";
    } else if (!none) {
        const char* why = "";
        if ((g_device1_disk = openCdDisc(name, why, 1)))
            g_device1_disc = g_disc_next;
        else
            Serial.printf("# DISC %s: %s\n", name, why);
    }
    uint32_t sectors = g_device1_disk ? g_device1_disk->sectors() : 0;
    g_device1_sectors.store(sectors, std::memory_order_relaxed);
    return sectors;
}

// Returns "" once the change has started, or why not: "nocdrom" or "busy"
// (SynthMinder's $SBC,1,DISC replies; the console prints them in words).
const char* startDiscChange(const char* name, Print& out) {
    if (!g_device0_cdrom && !g_device1_cdrom) {
        out.println("# DISC: no CD-ROM drive (device0 = cdrom or device1 = cdrom gives one)");
        return "nocdrom";
    }
    if (g_disc_step != DiscStep::None) {
        out.println("# DISC: a change is already under way");
        return "busy";
    }
    std::snprintf(g_disc_next, sizeof g_disc_next, "%s", name);
    requestDisc(0);  // out first: core 1 confirms once nothing for the old disc is under way
    g_disc_step = DiscStep::Ejecting;
    out.printf("# DISC ejecting, then %s\n", g_disc_next);
    return "";
}

void serviceDiscChange() {
    if (g_disc_step == DiscStep::None) return;
    if (g_disc_done.load(std::memory_order_acquire) != g_disc_seq.load(std::memory_order_relaxed)) return;
    if (g_disc_step == DiscStep::Ejecting) {
        uint32_t sectors = openDisc(g_disc_next);
        if (sectors) {
            requestDisc(sectors);
            g_disc_step = DiscStep::Inserting;
            return;
        }
        Serial.println("DISC none");
    } else {
        Serial.printf("DISC %s frames=%lu\n", g_disc_next,
                      (unsigned long)(g_disc_sectors.load(std::memory_order_relaxed) / atapi::SECTORS_PER_FRAME));
    }
    g_disc_step = DiscStep::None;
}

// Sets `key` (lower case) to `value` in HDDSYNTH.INI, keeping the rest of the
// file and its comments (the defaults if there is no file). Returns "" when
// saved, or why not: "card" (no card, or the file can't be read or written)
// or "key" (not a setting, or the file would be too long).
// HDDSYNTH.INI's device0 as SynthMinder or the console last set it, else as
// read at boot: it's served from the host's next hardware reset (armResetSwap),
// or the next start.
static char g_device0_next[drive_config::MAX_NAME + 1] = "";

// With device0 set to another image than the one served, the host's next
// RESET# brings it in (main_state.h, ResetSwap). Not for a CD-ROM, the test or
// bench disk, or the welcome disk: those wait for a restart, as before.
static void armResetSwap() {
    const uint8_t state = g_reset_swap.load(std::memory_order_acquire);
    if (state == RESET_SWAP_REQUESTED || state == RESET_SWAP_DONE) return;  // one under way
    const bool image = strcmp(g_disk_name, "image") == 0 && !g_device0_cdrom;
    const size_t n = strlen(g_device0_next);
    const bool img = n > 4 && strcasecmp(g_device0_next + n - 4, ".IMG") == 0;
    const bool other = img && strcasecmp(g_device0_next, g_image_name) != 0;
    g_reset_swap.store(image && other ? RESET_SWAP_ARMED : RESET_SWAP_NONE, std::memory_order_release);
}

// Core 0's part of a new disk at the host's reset: what's held for the old
// image written out, the new one opened (the old again if it can't be), its
// geometry, and its IDENTIFY built for core 1. The host waits: BSY.
void serviceResetSwap() {
    if (g_reset_swap.load(std::memory_order_acquire) != RESET_SWAP_REQUESTED) return;
    char old[drive_config::MAX_NAME + 1];
    std::snprintf(old, sizeof old, "%s", g_image_name);
    g_disk->flush();
    std::snprintf(g_config.device0, sizeof g_config.device0, "%s", g_device0_next);
    const char* why = "";
    if (useImage(g_config.device0, why)) {
        g_disk_name = "image";
        printfLine("DISK %s at the host's reset\n", g_config.device0);
    } else {
        printfLine("DISK %s can't be used (%s): %s again\n", g_config.device0, why, old);
        std::snprintf(g_config.device0, sizeof g_config.device0, "%s", old);
        useImage(g_config.device0, why);
    }
    g_device0_next[0] = 0;
    chooseGeometry();
    g_disk_sectors.store(g_disk->sectors(), std::memory_order_relaxed);
    const IdentifyInfo info = driveIdentity();
    ata::Device::identifyFor(info, g_reset_swap_identify);
    g_reset_swap_info = info;
    bus_observer::setIdentity(info);  // its IDENTIFY check, against the new disk's
    g_reset_swap.store(RESET_SWAP_DONE, std::memory_order_release);
}

static const char* setConfig(const char* key, const char* value, Print& out) {
    char* const text = scratch::a;
    char* const edited = scratch::b;
    size_t len = 0;
    bool truncated = false;
    if (!sd_card::readConfig(text, drive_config::MAX_FILE, len, truncated, out)) {
        if (!sd_card::mounted()) return "card";  // no card: readConfig said so
        if (sd_card::configExists()) {  // there, but unreadable: never overwrite it with the defaults
            out.printf("# E: can't read %s; nothing changed\n", drive_config::FILE_NAME);
            return "card";
        }
        len = strlen(drive_config::defaultFile());  // no file yet: start from the defaults
        memcpy(text, drive_config::defaultFile(), len);
    } else if (truncated) {
        out.printf("# E: %s is longer than %u bytes: edit it on a computer\n", drive_config::FILE_NAME,
                   unsigned(drive_config::MAX_FILE));
        return "card";
    }
    size_t n = drive_config::setKey(text, len, key, value, edited, drive_config::MAX_FILE);
    if (n == 0) {
        out.printf("# E: '%s' isn't a setting (image, mode, dma, write_behind, read_ahead, activity_link, seek_timing, isa, early_answers, iordy, register_table, sd_high_speed, geometry), "
                   "or the file would be too long\n", key);
        return "key";
    }
    if (!sd_card::writeConfig(edited, n, out)) return "card";
    drive_config::Config check;
    drive_config::parse(edited, n, check);
    out.printf("# saved %s = %s in %s; restart (!) to use it\n", key, value, drive_config::FILE_NAME);
    size_t shown = check.warning_count < drive_config::MAX_WARNINGS ? check.warning_count : drive_config::MAX_WARNINGS;
    for (size_t i = 0; i < shown; ++i) out.printf("CONFIG warning: %s\n", check.warnings[i]);
    return "";
}

// Console `E`: sets one setting in HDDSYNTH.INI from "key = value".
void editConfig(const char* line) {
    const char* eq = strchr(line, '=');
    char key[16] = "", value[drive_config::MAX_NAME + 1] = "";
    if (eq) {
        std::sscanf(line, " %15[^= \t]", key);
        std::sscanf(eq + 1, " %64[^\r\n]", value);
        for (char* p = key; *p; ++p) *p = char(tolower(*p));
        for (size_t n = strlen(value); n && (value[n - 1] == ' ' || value[n - 1] == '\t'); --n) value[n - 1] = '\0';
    }
    if (!key[0] || !value[0]) {
        Serial.println("# E: nothing changed; type key = value, e.g. device0 = GAMES.IMG");
        return;
    }
    if (!setConfig(key, value, Serial)[0] && strcmp(key, "device0") == 0) {
        std::snprintf(g_device0_next, sizeof g_device0_next, "%s", value);
        armResetSwap();
        if (g_reset_swap.load() == RESET_SWAP_ARMED) Serial.println("# device0: from the host's next reset (RESET#), or a restart");
    }
}

// ---- SynthMinder's requests for the drive (synthminder.h, bay_service.cpp) ----

// Prints nothing: what SynthMinder changes is reported by one SYNTHMINDER
// line, written only if USB serial has room (no PC may be reading it).
class Quiet : public Print {
public:
    size_t write(uint8_t) override { return 1; }
};


static bool hasExtension(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

// $SBQ,1,STATUS: one $SBV per value (docs/synthminder-protocol.md).
void bayStatus(synthminder::Reply& reply) {
    using synthminder::addValue;
    char text[64];
    addValue(reply, "firmware", FIRMWARE_VERSION);
#ifdef HDDPRO_PCB
    addValue(reply, "build", "pcb_drive");
#else
    addValue(reply, "build", "bb48_drive");
#endif
    // Device 0: what's served now, and what HDDSYNTH.INI names for the next start.
    const char* served = g_device0_cdrom                                          ? "cdrom"
                         : strcmp(g_disk_name, "image") == 0 || strcmp(g_disk_name, "bench") == 0 ? g_image_name
                                                                                    : g_disk_name;  // generated, welcome
    addValue(reply, "device0", served);
    const char* next = g_device0_next[0] ? g_device0_next : g_config.device0;
    if (!g_device0_cdrom && next[0] && strcasecmp(next, served) != 0) addValue(reply, "device0_next", next);
    addValue(reply, "device1", g_device1_cdrom ? "cdrom" : g_device1_disk ? g_config.device1 : "none");
    // The CD-ROM's disc: "-" for none.
    const char* disc = "-";
    if (g_device1_cdrom)
        disc = strcmp(g_device1_disc, "no disc") == 0 ? "-" : strcmp(g_device1_disc, "test disc") == 0 ? "test" : g_device1_disc;
    else if (g_device0_cdrom)
        disc = strcmp(g_disk_name, "no disc") == 0 ? "-" : strcmp(g_disk_name, "generated") == 0 ? "test" : g_image_name;
    addValue(reply, "disc", disc);
    // The bus program, and the fastest modes offered.
    const char* bus = ide_bus::busName();
    const char* program = ide_table::running()             ? "register table"
                          : strcmp(bus, "early") == 0      ? "early answers"
                          : strcmp(bus, "lane") == 0       ? "data lane"
                          : strcmp(bus, "noiordy") == 0    ? "no IORDY"
                                                           : "ide_bus";
    uint8_t mdma = g_mdma_mode.load();
    if (mdma != NO_DMA)
        std::snprintf(text, sizeof text, "PIO %u MDMA %u %s", unsigned(g_pio_mode.load()), unsigned(mdma), program);
    else
        std::snprintf(text, sizeof text, "PIO %u %s", unsigned(g_pio_mode.load()), program);
    addValue(reply, "bus", text);
    seekTimingSummary(text, sizeof text);
    addValue(reply, "seek_timing", text);
    std::snprintf(text, sizeof text, "%s %s", sd_card::busName(), sd_card::clockName());
    addValue(reply, "card", text);
    addValue(reply, "link", linkState());
}

// $SBC,1,DISC,<name>: as console `M`. "" once the change has started, or why
// not: "nocdrom", "busy" or "notfound" (not an ISO or CUE on the card).
const char* bayChangeDisc(const char* name) {
    if (!g_device0_cdrom && !g_device1_cdrom) return "nocdrom";
    bool none = strcmp(name, "-") == 0 || strcasecmp(name, "none") == 0;
    if (!none && strcasecmp(name, "test") != 0 &&
        (!(hasExtension(name, ".ISO") || hasExtension(name, ".CUE")) || !sd_card::fileExists(name)))
        return "notfound";
    Quiet quiet;
    const char* why = startDiscChange(none ? "none" : name, quiet);
    if (!why[0]) printfLine("SYNTHMINDER disc %s\n", none ? "none" : name);
    return why;
}

// $SBC,1,SET,<key>,<value>: as console `E`, from the next start: device0, and
// since 0.36.0 the settings $SBQ,1,SETTINGS lists (synthminder::checkSetting
// says which values each takes). "" when saved, or why not: "key", "value",
// "notfound" (no such image or disc on the card) or "card".
const char* baySet(const char* key, const char* value) {
    const synthminder::SettingCheck check = synthminder::checkSetting(key, value);
    if (check.needs == synthminder::SettingNeeds::Bad) return check.why;
    if (check.needs != synthminder::SettingNeeds::Nothing && !sd_card::fileExists(value)) return "notfound";
    char k[16];
    std::snprintf(k, sizeof k, "%s", key);
    for (char* p = k; *p; ++p) *p = char(tolower(*p));
    Quiet quiet;
    const char* why = setConfig(k, value, quiet);
    if (!why[0]) {
        if (strcmp(k, "device0") == 0) {
            std::snprintf(g_device0_next, sizeof g_device0_next, "%s", value);
            armResetSwap();  // from the host's next reset
        }
        printfLine("SYNTHMINDER set %s = %s (from the next start)\n", k, value);
    }
    return why;
}

// $SBQ,1,SETTINGS: one $SBS per setting SET may change, as HDDSYNTH.INI has it
// now (read afresh: from the next start) and as the drive runs; then, shown
// only, bus, dma, mode and geometry (since 0.36.0-dev.2).
void baySettings(synthminder::Reply& reply) {
    drive_config::Config file;
    drive_config::defaults(file);
    char* const text = scratch::a;
    size_t len = 0;
    bool truncated = false;
    Quiet quiet;
    if (sd_card::readConfig(text, drive_config::MAX_FILE, len, truncated, quiet) && !truncated)
        drive_config::parse(text, len, file);
    auto onOff = [](bool on) { return on ? "on" : "off"; };
    auto orNone = [](const char* s) { return s[0] ? s : "none"; };
    using synthminder::addSetting;
    addSetting(reply, "device1", orNone(file.device1), orNone(g_config.device1));
    addSetting(reply, "cdrom", orNone(file.cdrom), orNone(g_config.cdrom));
    addSetting(reply, "seek_timing", seek_timing::settingName(file.seek_timing),
               g_seek_on ? seek_timing::settingName(g_config.seek_timing) : "off");
    addSetting(reply, "activity_link", onOff(file.activity_link), onOff(g_link_on));
    addSetting(reply, "write_behind", onOff(file.write_behind), onOff(g_write_behind.load()));
    addSetting(reply, "read_ahead", onOff(file.read_ahead), onOff(g_read_ahead.load()));
    addSetting(reply, "sd_high_speed", onOff(file.sd_high_speed), onOff(g_config.sd_high_speed));

    // Shown only (SET refuses them): the PC must be right for each, so they're
    // changed in HDDSYNTH.INI, by hand. As the file has them, and as running.
    const char* bus_next = file.register_table          ? "table"
                           : file.bus != drive_config::Bus::Settings ? drive_config::busName(file.bus)
                           : file.early_answers         ? "early"
                                                        : "ide_bus";
    const char* bus_now = ide_table::running() ? "table" : ide_bus::busName();
    addSetting(reply, "bus", bus_next, bus_now);
    char now[16];
    uint8_t mdma = g_mdma_mode.load();
    if (mdma != NO_DMA)
        std::snprintf(now, sizeof now, "mdma%u", unsigned(mdma));
    else
        std::snprintf(now, sizeof now, "off");
    addSetting(reply, "dma", file.dma_auto ? "auto" : onOff(file.dma), now);
    std::snprintf(now, sizeof now, "pio%u", unsigned(g_pio_mode.load()));
    addSetting(reply, "mode", drive_config::modeName(file.mode), now);
    char next[20];
    if (file.geometry.cylinders)
        std::snprintf(next, sizeof next, "%u/%u/%u", unsigned(file.geometry.cylinders), unsigned(file.geometry.heads),
                      unsigned(file.geometry.sectors_per_track));
    else
        std::snprintf(next, sizeof next, "auto");
    uint32_t g = g_geometry.load(std::memory_order_relaxed);
    Geometry geo = g ? Geometry{uint16_t(g >> 16), uint16_t((g >> 8) & 0xFF), uint16_t(g & 0xFF)}
                     : defaultGeometry(g_disk_sectors.load(std::memory_order_relaxed));
    char used[20];
    std::snprintf(used, sizeof used, "%u/%u/%u", unsigned(geo.cylinders), unsigned(geo.heads),
                  unsigned(geo.sectors_per_track));
    addSetting(reply, "geometry", next, used);
}


// Console `S`: the settings and what came of them, after the card's own line.
void printConfig() {
    drive_config::Mode used = drive_config::modeSupported(g_config.mode) ? g_config.mode : drive_config::Mode::Auto;
    uint8_t mdma = g_mdma_mode.load();
    char mdma_text[8] = "none";
    if (mdma != NO_DMA) std::snprintf(mdma_text, sizeof mdma_text, "%u", unsigned(mdma));
    Serial.printf("CONFIG file=%s device0=%s mode=%s mode_used=%s pio_offered=%u mdma_offered=%s serving=%s contiguous=%s extents=%u parts=%u\n",
                  g_config_state,
                  g_config.device0, drive_config::modeName(g_config.mode), drive_config::modeName(used),
                  unsigned(g_pio_mode.load()), mdma_text, g_image_name,
                  sd_card::imageContiguous() ? "yes" : "no", sd_card::imageExtents(), sd_card::imageParts());
    if (g_device0_cdrom) {
        if (g_image_note[0]) Serial.printf("CONFIG cdrom not used: %s\n", g_image_note);
        Serial.printf("CONFIG device0=%s kind=cdrom disc=%s frames=%lu\n", g_config.device0,
                      g_disk_name[0] == 'g' ? "test disc" : g_disk_name[0] == 'n' ? "no disc" : g_image_name,
                      (unsigned long)(g_disk->sectors() / atapi::SECTORS_PER_FRAME));
    } else if (g_image_note[0]) {
        Serial.printf("CONFIG device0 not used: %s\n", g_image_note);
    }
    if (g_device1_cdrom)
        Serial.printf("CONFIG device1=%s kind=cdrom disc=%s frames=%lu\n", g_config.device1, g_device1_disc,
                      (unsigned long)(g_device1_sectors.load() / atapi::SECTORS_PER_FRAME));
    if (g_device1_disk && g_device1_kind[0] != 'c') {
        Serial.printf("CONFIG device1=%s kind=%s sectors=%lu\n", g_config.device1, g_device1_kind,
                      (unsigned long)g_device1_disk->sectors());
        if (g_device1_disk == &g_folder_disk) {
            static const char* why[] = {"", ": too many entries", ": the root folder is full", ": over 2 GB"};
            Serial.printf("CONFIG folder entries=%u/%u cluster_bytes=%lu read_only=yes%s%s\n",
                          unsigned(g_folder.count()), unsigned(g_folder_capacity),
                          (unsigned long)g_folder.clusterBytes(),
                          g_folder.truncated() == folder_disk::Truncated::No ? "" : " cut_short",
                          why[int(g_folder.truncated())]);
        }
    } else if (g_device1_note[0]) {
        Serial.printf("CONFIG device1 not used: %s\n", g_device1_note);
    }
    Serial.printf("CONFIG dmarq=%s dma=%s write_behind=%s read_ahead=%s activity_link=%s isa=%s early_answers=%s iordy=%s register_table=%s\n",
                  g_dmarq_wired.load() ? "wired" : "absent", g_config.dma_auto ? "auto" : drive_config::onOff(g_config.dma),
                  drive_config::onOff(g_config.write_behind), drive_config::onOff(g_config.read_ahead),
                  drive_config::onOff(g_config.activity_link), drive_config::onOff(g_config.isa),
                  drive_config::onOff(g_config.early_answers), drive_config::onOff(g_config.iordy),
                  drive_config::onOff(g_config.register_table));
    if (g_config.dma_auto && !g_bus_auto.load())
        Serial.printf("CONFIG dma = auto: no DMA offered, as it needs bus = auto (or console P a) to learn the PC\n");
    else if (g_config.dma_auto)
        Serial.printf("CONFIG dma = auto: %s\n", g_dma_added.load() ? "multiword DMA offered, the PC learned"
                                                 : "offered once bus = auto has learned the PC");
    if (g_config.register_table && !g_table.load()) {
        const char* why = busRequested() ? "the console's bus program (P) is used instead"
                          : (g_device0_cdrom || g_device1_cdrom) ? "a CD-ROM is configured, so ide_bus runs"
                          : g_config.isa ? "isa = on keeps its own program"
                                         : "not started";
        Serial.printf("CONFIG register_table not used: %s\n", why);
    }
    if (g_table.load() && g_config.dma && g_dmarq_wired.load())
        Serial.printf("CONFIG register table: no DMA offered (dma = on is for the other bus programs, so far)\n");
    if (g_pio_planned.load(std::memory_order_acquire)) {
        const pio_plan::Plan& p = g_pio_plan;
        const bool diag = !sd_card::keptDiagnosticsBlock();  // a disc put in later may have taken their block
        Serial.printf("CONFIG pio card=%s dma=%s iocs16=%s iordy_timer=%s bus_observer=%s bus_timing=%s%s%s%s\n",
                      sd_card::busName(), drive_config::onOff(p.ide_dma), drive_config::onOff(p.iocs16),
                      drive_config::onOff(p.iordy_timer && diag), drive_config::onOff(p.bus_observer && diag),
                      drive_config::onOff(p.bus_timing), p.why ? ": " : "", p.why ? p.why : "",
                      diag ? "" : " (the card took the diagnostics' block for a disc, until a restart)");
    }
    if (busRequested())
        Serial.printf("CONFIG bus program set from the console (P%c) until a power cycle\n", busRequested());
    if (g_device0_cdrom || g_device1_cdrom)
        Serial.printf(g_early.load() ? "CONFIG with a CD-ROM as device %d on early answers (console P e): the USB adapter can't detect it\n"
                                     : "CONFIG with a CD-ROM as device %d, ide_bus\n",
                      g_device0_cdrom ? 0 : 1);
    {
        Geometry g = startGeometry(driveIdentity());
        Serial.printf("CONFIG geometry=%u/%u/%u (%s)%s\n", unsigned(g.cylinders), unsigned(g.heads),
                      unsigned(g.sectors_per_track), g_geometry_from,
                      g_geometry_trimmed ? ", cylinders cut to fit the disk" : "");
    }
    printSeekConfig();
    if (drive_config::dmaRefusedForWiring(g_config.mode, g_dmarq_wired.load()))
        Serial.printf("CONFIG warning: mode = %s needs DMARQ wired (IDE pin 21 to GPIO 31); offering PIO only\n",
                      drive_config::modeName(g_config.mode));
    size_t shown = g_config.warning_count < drive_config::MAX_WARNINGS ? g_config.warning_count : drive_config::MAX_WARNINGS;
    for (size_t i = 0; i < shown; ++i) Serial.printf("CONFIG warning: %s\n", g_config.warnings[i]);
    if (g_config.warning_count > shown)
        Serial.printf("CONFIG warning: and %u more\n", unsigned(g_config.warning_count - shown));
    if (strcmp(g_config_state, "truncated") == 0)
        Serial.printf("CONFIG warning: only the first %u bytes of the file were read\n", unsigned(drive_config::MAX_FILE));
}

#endif  // FIRMWARE_SLICE != 0
