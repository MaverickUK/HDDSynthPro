#include "sd_card.h"

#include <SPI.h>
#include <cstdio>
#include <SdFat.h>

#include "card_retry.h"
#include "config.h"
#include "drive_config.h"
#include "image_parts.h"

namespace sd_card {

static SdFs s_sd;
static FsFile s_image;
static const char* s_open_name = nullptr;  // which image s_image is
static const char* s_open_error = "";

static const char* cardType(uint8_t type) {
    switch (type) {
        case SD_CARD_TYPE_SD1: return "SD1";
        case SD_CARD_TYPE_SD2: return "SD2";
        case SD_CARD_TYPE_SDHC: return "SDHC/SDXC";
        default: return "?";
    }
}

static const char* fsType(uint8_t fat_type) {
    switch (fat_type) {
        case FAT_TYPE_FAT12: return "FAT12";
        case FAT_TYPE_FAT16: return "FAT16";
        case FAT_TYPE_FAT32: return "FAT32";
        case FAT_TYPE_EXFAT: return "exFAT";
        default: return "none";
    }
}

// Puts SPI0 on the SD pins. If a pin were refused, SPI would fall back to
// its default pins, which on this board are IDE data lines: don't start at all.
static bool spiPins() {
    return SPI.setSCK(cfg::PIN_SD_CLK) && SPI.setTX(cfg::PIN_SD_CMD) && SPI.setRX(cfg::PIN_SD_DAT0);
}

static bool startSpi() {
    if (!spiPins()) return false;
    SdSpiConfig config(cfg::PIN_SD_DAT3, DEDICATED_SPI, SD_SCK_MHZ(cfg::SD_SPI_MHZ), &SPI);
    return s_sd.begin(config);
}

// Whether the card runs over SDIO (the PCB build, when it started that way).
static bool s_on_sdio = false;
#if SD_SDIO
// SDIO may be tried: until the PIO blocks were shared out without it
// (sharePio), or a card that was in wouldn't start over SDIO at boot.
static bool s_sdio_allowed = true;
// The card is started for the first time: falling back to SPI is allowed.
static bool s_booting = true;
// A card that was in wouldn't start over SDIO at boot, and runs over SPI.
static bool s_sdio_refused = false;
// The PCB's card detect switch: low with a card in.
static bool cardDetected() { return !gpio_get(cfg::PIN_SD_CD); }
// Running over SPI after SDIO was refused, the card was taken out: a card put
// back is in SD mode and starts over SDIO after a restart, so nothing may send
// it the SPI CMD0 that would put it back in SPI mode (wantsSdioRestart()).
static bool s_out_since_refused = false;
// No image on the card is served (the test disk), so SDIO was ended at boot
// and its PIO block went to the diagnostics; the card waits in SD mode. A
// card operation borrows the block (s_lend stops the diagnostics), starts
// SDIO, and gives it back when done (giveBack).
static bool s_parked = false;
static bool s_borrowed = false;
static bool (*s_lend)() = nullptr;
static void (*s_reclaim)() = nullptr;
#endif

// Starts the card and mounts its volume (a fresh volume: whatever SdFat had
// cached from a card before is dropped). The PCB build starts it over SDIO;
// a card that's in but won't (most likely left in SPI mode, which only a
// power cycle ends) runs over SPI instead, if that happens at boot.
static bool startCard() {
#if SD_SDIO
    if (s_sdio_allowed) {
        if (s_sd.begin(SdioConfig(cfg::PIN_SD_CLK, cfg::PIN_SD_CMD, cfg::PIN_SD_DAT0, cfg::SD_SDIO_CLKDIV))) {
            s_on_sdio = true;
            return true;
        }
        s_sd.end();  // frees the PIO block SdFat's driver claimed, even when it failed
        s_on_sdio = false;
        if (!s_booting || !cardDetected()) return false;  // later: try SDIO again next time
        s_sdio_allowed = false;
        s_sdio_refused = true;
    }
    if (s_out_since_refused) return false;  // keep it out of SPI mode: the drive restarts for SDIO
#endif
    return startSpi();
}

// Device 1's open image (openDevice1Image), declared here because restarting
// the card closes it too; it's reopened by name at its next read or write.
static FsFile s_device1_image;
// Times the card was started again after it stopped answering, from any
// path: device 0's reads (reattach), device 1's, or `S` and `M` finding it
// silent. printCardChanges() prints a CARD line for each.
static uint32_t s_restarts = 0;

// Mounts the card's volume, unless it already is.
static bool mount(Print& out) {
    if (s_sd.fatType()) return true;
#if SD_SDIO
    if (s_parked) {
        if (!s_lend || !s_lend()) {
            out.println("SD error: the card's PIO block couldn't be borrowed");
            return false;
        }
        if (s_sd.begin(SdioConfig(cfg::PIN_SD_CLK, cfg::PIN_SD_CMD, cfg::PIN_SD_DAT0, cfg::SD_SDIO_CLKDIV))) {
            s_borrowed = true;
            return true;
        }
        out.printf("SD error: no card, or no FAT volume on it (code %u, data %u)\n",
                   unsigned(s_sd.sdErrorCode()), unsigned(s_sd.sdErrorData()));
        s_sd.end();
        s_reclaim();
        return false;
    }
#endif
    if (!startCard()) {
        out.printf("SD error: no card, or no FAT volume on it (code %u, data %u)\n",
                   unsigned(s_sd.sdErrorCode()), unsigned(s_sd.sdErrorData()));
        return false;
    }
    return true;
}

// Ends a borrowed SDIO session (mount() while parked) and gives the PIO block
// back to the diagnostics. Every public function that may mount the card
// holds a Lend for its duration.
static void giveBack() {
#if SD_SDIO
    if (!s_borrowed) return;
    s_sd.end();
    s_borrowed = false;
    s_reclaim();
#endif
}
struct Lend {
    ~Lend() { giveBack(); }
};

// "open" (being served), "present" or "none".
static const char* imageState(const char* name) {
    if (s_image.isOpen() && s_open_name && strcasecmp(s_open_name, name) == 0) return "open";
    return s_sd.exists(name) ? "present" : "none";
}

static bool wakeIfSilent(Print& out);  // below, with the card watch

void printInfo(Print& out) {
    Lend lend;
    if (!mount(out) || !wakeIfSilent(out)) return;
    uint32_t sectors = s_sd.card()->sectorCount();
    out.printf("SD ok type=%s size_mb=%lu fs=%s image=%s bench=%s config=%s bus=%s\n", cardType(s_sd.card()->type()),
               (unsigned long)(sectors / 2048), fsType(s_sd.fatType()),
               imageState(IMAGE_NAME), imageState(BENCH_IMAGE_NAME),
               s_sd.exists(drive_config::FILE_NAME) ? "present" : "none", busName());
#if SD_SDIO
    if (s_sdio_refused)
        out.println("SD warning: the card didn't start over SDIO, so it runs over SPI (about a quarter of the speed). "
                    "It was most likely left in SPI mode: take it out and put it back for SDIO.");
#endif
}

const char* busName() {
#if SD_SDIO
    if (s_parked) return "sdio-idle";
#endif
    return s_on_sdio ? "sdio" : "spi";
}

void setPioLender(bool (*lend)(), void (*reclaim)()) {
#if SD_SDIO
    s_lend = lend;
    s_reclaim = reclaim;
#else
    (void)lend;
    (void)reclaim;
#endif
}

bool wantsSdioRestart() {
#if SD_SDIO
    if (!s_sdio_refused) return false;
    if (!cardDetected()) {
        s_out_since_refused = true;
        return false;
    }
    return s_out_since_refused;
#else
    return false;
#endif
}
bool sdio() { return s_on_sdio; }

static bool s_folder_served = false;

void sharePio() {
#if SD_SDIO
    s_booting = false;
    if (!s_on_sdio) {
        s_sdio_allowed = false;  // its block went to something else
        return;
    }
    // Nothing on the card served (the test disk, or the welcome disk with no
    // image): park the card, so the diagnostics get its block.
    if (!s_image.isOpen() && !s_device1_image.isOpen() && !s_folder_served && s_lend && s_reclaim) {
        s_sd.end();
        s_on_sdio = false;
        s_parked = true;
    }
#endif
}

// ---- Watching the card (media_watch.h) ---------------------------------------

static media_watch::Watch s_watch;
static const char* s_served_name = nullptr;  // the image being served, kept while the card is out
static oflag_t s_image_flags = O_RDWR;  // how it was opened: reopened the same way (read-only for a disc)
// A split image (image_parts.h): its parts, all of the image's sectors, its
// size in bytes (media_watch's identity), and why it couldn't be served.
static unsigned s_parts = 1;
static uint32_t s_image_sectors = 0;
static uint64_t s_image_bytes = 0;
static const char* s_split_error = nullptr;

static uint64_t s_served_size = 0;           // its size, which the drive told the host

// Whether a card answers CMD0 (go idle, and into SPI mode), the first step of
// starting one. Sent by hand at 400 kHz, because SdFat's start-up retries it
// for 2 s when there's no card; this takes about half a millisecond. With no
// card the pull-up on DAT0 reads 0xFF. Only while the card is lost: CMD0
// resets a card, so SdFat must start it again afterwards.
static bool cardAnswers() {
#if SD_SDIO
    // Never an SPI CMD0 for a card served over SDIO: it would leave the card
    // in SPI mode. The PCB has card detect; starting the card says the rest.
    if (s_sdio_allowed) return cardDetected();
    if (s_out_since_refused) return false;  // no SPI CMD0 for a card that could run over SDIO
#endif
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(cfg::PIN_SD_DAT3, HIGH);
    for (int i = 0; i < 10; ++i) SPI.transfer(0xFF);  // 80 clocks with CS high wake a card just put in
    digitalWrite(cfg::PIN_SD_DAT3, LOW);
    static const uint8_t CMD0[6] = {0x40, 0, 0, 0, 0, 0x95};
    for (uint8_t b : CMD0) SPI.transfer(b);
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10 && r1 == 0xFF; ++i) r1 = SPI.transfer(0xFF);
    digitalWrite(cfg::PIN_SD_DAT3, HIGH);
    SPI.transfer(0xFF);
    SPI.endTransaction();
    return r1 == 0x01;  // idle
}

static void checkContiguous();  // below, with s_contiguous

// The card in, and the open image on it.
static bool identify(media_watch::Identity& id) {
    cid_t cid;
    if (!s_sd.card()->readCID(&cid)) return false;
    id.maker = cid.mid;
    id.serial = cid.psn();
    id.size = s_image_bytes;  // all of a split image's parts (checkContiguous comes first)
    id.first_sector = uint32_t(s_image.firstSector());
    return s_image.getModifyDateTime(&id.date, &id.time);
}

// While the card is lost: starts a card that answers and reopens the image.
// True if it's the card and image served, which can then be used again.
static bool reattach(uint32_t now) {
    if (!cardAnswers()) return false;
    // The card is idle now and accepts no writes, so nothing SdFat still has
    // cached from before can reach it as the file is closed.
    s_image.close();
    s_device1_image.close();  // reopened by name at its next read (device1Attempt)
    if (!startCard()) {
        s_watch.cameBackWithout("can't mount", now);
        return false;
    }
    if (!s_image.open(s_served_name, s_image_flags)) {
        s_watch.cameBackWithout(s_sd.exists(s_served_name) ? "can't open" : "image not found", now);
        return false;
    }
    checkContiguous();  // first: a split image's size is its parts'
    media_watch::Identity id;
    if (s_split_error) {
        s_watch.cameBackWithout(s_split_error, now);
    } else if (!identify(id)) {
        s_watch.cameBackWithout("can't read the card's ID", now);
    } else if (s_watch.cameBack(id, now) == media_watch::State::Present) {
        ++s_restarts;
        return true;
    }
    s_image.close();
    return false;
}

// An image read, write or flush. Refused at once while the card is lost or
// changed. One that fails is retried once if the card restarts as the same
// card: it may have been pulled and put back while the drive was idle, or
// have stopped answering for a moment.
template <typename Op>
static bool attempt(Op op) {
    if (!s_watch.usable()) return false;
    if (op()) return true;
    uint32_t now = millis();
    s_watch.failed(now);
    if (reattach(now) && op()) return true;
    s_watch.failed(millis());  // the retry failed too
    return false;
}

// The card's operations, for card_retry's decisions (card_retry.h).
struct SdCardOps {
    uint32_t now() { return millis(); }
    bool answers() { return cardAnswers(); }
    bool start() { return startCard(); }
    bool reattach(uint32_t now) { return ::sd_card::reattach(now); }
    void closeDevice1() { s_device1_image.close(); }
    bool reopenDevice1();  // below, with device 1's image
    void countRestart() { ++s_restarts; }
};
static SdCardOps s_card_ops;

// The card stopped answering on a path other than device 0's image reads
// (device 1's disk, `S`, `M`): restarts it (card_retry::restart).
static bool restartCard() { return card_retry::restart(s_watch, s_card_ops); }

// Whether the mounted card has stopped answering: SdFat still has its volume,
// but the card doesn't give its size. So it was pulled out, or put back while
// the drive ran (it then wakes in SD mode, not SPI), with no image read since
// to notice. Asks the card (CMD9), a few hundred microseconds.
static bool cardSilent() {
    if (!s_sd.fatType()) return false;
#if SD_SDIO
    // SDIO's driver gives the size it read at start-up; CMD13 asks the card.
    // The read in progress is ended first (CMD12): the driver leaves a
    // multi-sector read open between calls and takes the next sector from it
    // without a command, so after a CMD13 in the middle it returned other
    // sectors' data, CRC good and no error (8 Nov: 19 of 20 lookups failed).
    if (s_on_sdio) return !cardDetected() || !s_sd.card()->syncDevice() || s_sd.card()->status() == 0xFFFFFFFF;
#endif
    return s_sd.card()->sectorCount() == 0;
}

// Restarts the card if it has stopped answering; for `S` and `M`, before they
// look for files. False if it's silent and couldn't be restarted.
static bool wakeIfSilent(Print& out) {
    if (!cardSilent()) return true;
    if (restartCard()) {
        out.println("SD restarted: it had stopped answering");
        return true;
    }
    out.println("SD error: the card doesn't answer, and couldn't be restarted");
    return false;
}

uint32_t restarts() { return s_restarts; }

const media_watch::Watch& media() { return s_watch; }

void serviceMedia() {
    uint32_t now = millis();
    if (!s_watch.due(now)) return;
    if (s_watch.state() == media_watch::State::Lost) {
        reattach(now);
    } else {
        // A different card: still in? (SDIO's driver gives the CID it read
        // at start-up, so the PCB asks card detect.)
#if SD_SDIO
        if (s_on_sdio) {
            if (!cardDetected()) s_watch.gone(now);
            return;
        }
#endif
        cid_t cid;
        if (!s_sd.card()->readCID(&cid)) s_watch.gone(now);
    }
}

// The open image as the drive's ImageFile. Its size is the one the host was
// given, even while the card is out.
static bool s_contiguous = false;

// The image's runs of clusters on the card, so reads and writes go straight to
// card sectors, whatever the fragmentation. Through SdFat's file layer a seek
// backwards walks the cluster chain from the file's start unless SdFat knows
// the file is one run: 172 ms a read in a 4 GB image (a Pentium's FC comparing
// two files, 0.23.9). Built at open; empty when unavailable (a fragmented exFAT
// image, more than MAX_EXTENTS runs, a FAT read failing), and then the file
// layer is used as before.
struct Extent {
    uint32_t file_sector;  // the image's sector the run starts at
    uint32_t card_sector;  // where it is on the card
    uint32_t sectors;
};
constexpr size_t MAX_EXTENTS = 256;
static Extent s_extents[MAX_EXTENTS];
static size_t s_extent_count = 0;

static uint32_t load32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

// A file's FAT32 chain, from its first cluster, appended to s_extents as the
// image's sectors from `base` (a split image's later parts start past the
// earlier ones). False if it can't be.
static bool walkFat32(FsFile& f, uint32_t base, uint32_t sectors) {
    const uint32_t spc = s_sd.bytesPerCluster() / 512;
    const Sector_t data = s_sd.dataStartSector(), fat = s_sd.fatStartSector();
    if (spc == 0 || f.firstSector() < data) return false;
    uint32_t cluster = uint32_t((f.firstSector() - data) / spc) + 2;
    static uint8_t sector[512];
    Sector_t cached = ~Sector_t(0);
    uint32_t file_sector = base;
    const uint32_t image_sectors = base + sectors;
    while (file_sector < image_sectors) {
        uint32_t start = cluster, count = 1, next;
        for (;;) {  // follow the run
            Sector_t s = fat + cluster / 128;
            if (s != cached) {
                if (!s_sd.card()->readSector(s, sector)) return false;
                cached = s;
            }
            next = load32(sector + (cluster % 128) * 4) & 0x0FFFFFFF;
            if (next != cluster + 1) break;
            cluster = next;
            ++count;
        }
        if (s_extent_count == MAX_EXTENTS) return false;
        s_extents[s_extent_count++] = {file_sector, uint32_t(data + Sector_t(start - 2) * spc), count * spc};
        file_sector += count * spc;
        if (next < 2 || next >= 0x0FFFFFF8) break;  // the chain's end
        cluster = next;
    }
    return file_sector >= image_sectors;
}

static bool mapAgrees(FsFile& f, uint32_t base, uint32_t sectors, size_t first_extent);  // below

// One file's runs into s_extents, as the image's sectors from `base`, checked
// against the file layer. False if it can't be mapped.
static bool mapPart(FsFile& f, uint32_t base, uint32_t sectors, bool& contiguous) {
    size_t first_extent = s_extent_count;
    Sector_t first = 0, last = 0;
    contiguous = f.contiguousRange(&first, &last);
    if (contiguous) {
        if (s_extent_count == MAX_EXTENTS) return false;
        s_extents[s_extent_count++] = {base, uint32_t(first), uint32_t(last - first + 1)};
    } else if (s_sd.fatType() != FAT_TYPE_FAT32 || !walkFat32(f, base, sectors)) {
        return false;
    }
    return mapAgrees(f, base, sectors, first_extent);
}

// Maps the open image (s_extents), with any further parts (NAME.I01...), and
// tells SdFat if it's one run, so its own seeks are sums too. A single file
// that can't be mapped is served through the file layer; a split image can
// only be served through the map, so then s_split_error says why not.
static void checkContiguous() {
    s_extent_count = 0;
    s_parts = 1;
    s_split_error = nullptr;
    s_image.sync();  // nothing of the image left in SdFat's cache: from here on it's written directly
    uint64_t bytes = s_image.fileSize();
    uint32_t sectors = uint32_t((bytes + 511) / 512);
    bool mapped_ok = mapPart(s_image, 0, sectors, s_contiguous);
    s_image_sectors = sectors;
    s_image_bytes = bytes;
    for (unsigned n = 1; n < image_parts::MAX_PARTS && s_served_name; ++n) {
        char name[drive_config::MAX_NAME + 8];
        if (!image_parts::partName(s_served_name, n, name, sizeof name) || !s_sd.exists(name)) break;
        if (n == 1) s_contiguous = false;  // SdFat's seeks within one file are no help now
        if (s_image_bytes % 512) {
            s_split_error = "a part before the last isn't a whole number of sectors";
            break;
        }
        FsFile part;
        bool contiguous;
        if (!part.open(name, O_RDONLY)) {
            s_split_error = "can't open a part";
            break;
        }
        uint64_t part_bytes = part.fileSize();
        uint32_t part_sectors = uint32_t((part_bytes + 511) / 512);
        if (!mapped_ok || !mapPart(part, s_image_sectors, part_sectors, contiguous)) {
            s_split_error = "can't map its parts (a FAT32 or exFAT card, under 256 runs in all)";
            part.close();
            break;
        }
        part.close();
        s_image_sectors += part_sectors;
        s_image_bytes += part_bytes;
        s_parts = n + 1;
    }
    if (!mapped_ok || s_split_error) s_extent_count = 0;
}

// Reads or writes whole sectors through s_extents. False if the map can't
// serve it (none, or not whole sectors): then the file layer does.
template <class Io>
static bool mapped(uint64_t offset, size_t bytes, Io io) {
    if (s_extent_count == 0 || (offset | bytes) % 512) return false;
    uint32_t sector = uint32_t(offset / 512), left = uint32_t(bytes / 512), done = 0;
    size_t lo = 0, hi = s_extent_count;  // the last extent starting at or before `sector`
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        (s_extents[mid].file_sector <= sector ? lo : hi) = mid;
    }
    for (size_t e = lo; left && e < s_extent_count; ++e) {
        const Extent& x = s_extents[e];
        if (sector < x.file_sector || sector >= x.file_sector + x.sectors) return false;
        uint32_t n = x.file_sector + x.sectors - sector;
        if (n > left) n = left;
        if (!io(x.card_sector + (sector - x.file_sector), done, n)) return false;
        sector += n, done += n, left -= n;
    }
    return left == 0;
}

// A wrong map would serve the wrong sectors without a word, so before using it
// a few sectors are read both ways (the map, SdFat's file layer) and compared:
// each of the first runs and the last one at its start, the middle, the end.
static bool s_map_checked = false;
static bool mapAgrees(FsFile& f, uint32_t base, uint32_t sectors, size_t first_extent) {
    static uint8_t a[512], b[512];
    if (sectors == 0) return true;
    uint32_t samples[8];  // the image's sectors (within this part: from `base`)
    size_t n = 0;
    for (size_t e = first_extent; e < s_extent_count && n < 4; ++e) samples[n++] = s_extents[e].file_sector;
    samples[n++] = s_extents[s_extent_count - 1].file_sector;
    samples[n++] = base + sectors / 2;
    samples[n++] = base + sectors - 1;
    for (size_t i = 0; i < n; ++i) {
        uint32_t lba = samples[i];
        bool ok = mapped(uint64_t(lba) * 512, 512, [&](Sector_t at, uint32_t, uint32_t count) {
            return s_sd.card()->readSectors(at, a, count);
        });
        if (!ok || !f.seekSet(uint64_t(lba - base) * 512) || f.read(b, 512) != 512 || memcmp(a, b, 512) != 0)
            return false;
    }
    s_map_checked = true;
    return true;
}

class SdImageFile : public ImageFile {
public:
    uint64_t size() const override { return s_served_size; }
    bool readAt(uint64_t offset, void* data, size_t bytes) override {
        return attempt([&] {
            if (s_extent_count && (offset | bytes) % 512 == 0)
                return mapped(offset, bytes, [&](Sector_t at, uint32_t done, uint32_t n) {
                    return s_sd.card()->readSectors(at, static_cast<uint8_t*>(data) + done * 512, n);
                });
            return s_image.seekSet(offset) && s_image.read(data, bytes) == int(bytes);
        });
    }
    bool writeAt(uint64_t offset, const void* data, size_t bytes) override {
        return attempt([&] {
            if (s_extent_count && (offset | bytes) % 512 == 0)
                return mapped(offset, bytes, [&](Sector_t at, uint32_t done, uint32_t n) {
                    return s_sd.card()->writeSectors(at, static_cast<const uint8_t*>(data) + done * 512, n);
                });
            return s_image.seekSet(offset) && s_image.write(data, bytes) == bytes;
        });
    }
    // SdFat's cache to the card, then waits until the card isn't busy.
    bool sync() override {
        return attempt([] { return s_image.sync(); });
    }
};
static SdImageFile s_image_file;

ImageFile* openImage(const char* name, Print& log, bool read_only) {
    if (s_image.isOpen()) s_image.close();
    s_open_name = nullptr;
    s_watch = media_watch::Watch();
    if (!mount(log)) {
        s_open_error = "no card";
        return nullptr;
    }
    if (!s_sd.exists(name)) {
        s_open_error = "not found";
        return nullptr;
    }
    s_image_flags = read_only ? O_RDONLY : O_RDWR;
    if (!s_image.open(name, s_image_flags)) {
        s_open_error = "can't open";
        log.printf("SD error: can't open %s\n", name);
        return nullptr;
    }
    s_served_name = name;  // checkContiguous looks for the parts of a split image by it
    checkContiguous();
    if (s_split_error) {
        s_image.close();
        s_served_name = nullptr;
        s_open_error = s_split_error;
        log.printf("SD error: %s is split across files, but %s\n", name, s_split_error);
        return nullptr;
    }
    media_watch::Identity id;
    if (!identify(id)) {
        s_image.close();
        s_served_name = nullptr;
        s_open_error = "can't open";
        log.printf("SD error: can't read the card's ID\n");
        return nullptr;
    }
    s_open_name = name;
    s_served_size = id.size;
    s_watch.serve(id);
    return &s_image_file;
}

const char* openError() { return s_open_error; }

bool imageContiguous() { return s_contiguous; }

unsigned imageExtents() { return unsigned(s_extent_count); }

unsigned imageParts() { return s_parts; }

// Prints nothing: for card operations whose caller reports the outcome itself.
class NullPrint : public Print {
public:
    size_t write(uint8_t) override { return 1; }
};

bool configExists() {
#if SD_SDIO
    if (s_parked) {  // the card is in but idle: borrow it to look (`C` must never take "parked" for "missing")
        Lend lend;
        NullPrint quiet;
        return mount(quiet) && s_sd.exists(drive_config::FILE_NAME);
    }
#endif
    return s_sd.fatType() && s_sd.exists(drive_config::FILE_NAME);
}

bool mounted() {
#if SD_SDIO
    if (s_parked) return true;  // mounted at boot, idle since: mount() borrows it again
#endif
    return s_sd.fatType() != 0;
}

static bool readConfigOnce(char* buf, size_t size, size_t& len, bool& truncated) {
    len = 0;
    truncated = false;
    FsFile f;
    if (!f.open(drive_config::FILE_NAME, O_RDONLY)) return false;
    int n = f.read(buf, size);
    truncated = n >= 0 && uint64_t(n) < f.fileSize();
    f.close();
    if (n < 0) return false;
    len = size_t(n);
    return true;
}

// Retried once after restarting the card, as writeConfig is: after the card
// was pulled and put back unnoticed, reads failed too, and console E took
// the file for missing and rewrote it from the defaults (2026-10-31).
bool readConfig(char* buf, size_t size, size_t& len, bool& truncated, Print& log) {
    Lend lend;
    if (!mount(log)) return false;
    if (readConfigOnce(buf, size, len, truncated)) return true;
    if (!s_sd.exists(drive_config::FILE_NAME) && s_sd.sdErrorCode() == 0) return false;  // simply not there
    bool restarted = s_served_name ? reattach(millis()) : startCard();
    return restarted && readConfigOnce(buf, size, len, truncated);
}

static bool writeConfigOnce(const char* text, size_t len) {
    FsFile f;
    if (!f.open(drive_config::FILE_NAME, O_RDWR | O_CREAT | O_TRUNC)) return false;
    bool ok = f.write(text, len) == len && f.sync();
    f.close();
    return ok;
}

// Console E's write. One that fails is retried once after restarting the
// card, as image reads and writes are (attempt): it may have been pulled and
// put back while the drive was idle, which nothing noticed (2026-10-31: the
// write failed until a restart), or have stopped answering for a moment.
bool writeConfig(const char* text, size_t len, Print& out) {
    Lend lend;
    if (!mount(out)) return false;
    if (writeConfigOnce(text, len)) return true;
    unsigned code = unsigned(s_sd.sdErrorCode()), data = unsigned(s_sd.sdErrorData());
    bool restarted = s_served_name ? reattach(millis()) : startCard();
    if (restarted && writeConfigOnce(text, len)) {
        out.printf("# %s written after restarting the card (the first try failed: code %u, data %u)\n",
                   drive_config::FILE_NAME, code, data);
        return true;
    }
    out.printf("SD error: can't write %s (code %u, data %u; after restarting the card: %s, code %u, data %u)\n",
               drive_config::FILE_NAME, code, data, restarted ? "failed again" : "it didn't restart",
               unsigned(s_sd.sdErrorCode()), unsigned(s_sd.sdErrorData()));
    return false;
}

bool createImage(const char* name, uint32_t sectors, Print& out) {
    Lend lend;
    if (!mount(out)) return false;
    if (s_sd.exists(name)) {
        out.printf("# %s already exists: left as it is\n", name);
        return false;
    }
    uint64_t bytes = uint64_t(sectors) * 512;
    uint64_t free_bytes = uint64_t(s_sd.freeClusterCount()) * s_sd.bytesPerCluster();
    if (free_bytes < bytes + (1u << 20)) {
        out.printf("SD error: %lu MB free, %lu MB needed\n", (unsigned long)(free_bytes >> 20),
                   (unsigned long)(bytes >> 20));
        return false;
    }
    FsFile file;
    if (!file.open(name, O_RDWR | O_CREAT | O_EXCL)) {
        out.printf("SD error: can't create %s\n", name);
        return false;
    }
    // One contiguous run of clusters, then zeros: a blank disk, which is
    // what a host expects before it partitions it.
    bool ok = file.preAllocate(bytes);
    static uint8_t zeros[4096];
    uint64_t written = 0;
    while (ok && written < bytes) {
        ok = file.write(zeros, sizeof zeros) == sizeof zeros;
        written += sizeof zeros;
        if ((written & ((8u << 20) - 1)) == 0) out.printf("# %lu MB\n", (unsigned long)(written >> 20));
    }
    ok = ok && file.sync();
    file.close();
    if (!ok) {
        s_sd.remove(name);  // our own half-made file, nothing else
        out.printf("SD error: writing %s failed (code %u); removed it\n", name,
                   unsigned(s_sd.sdErrorCode()));
        return false;
    }
    out.printf("# created %s, %lu sectors; reboot the drive to use it\n", name,
               (unsigned long)sectors);
    return true;
}

// ---- Device 1's disk ----------------------------------------------------------

bool readFileStart(const char* name, char* buf, size_t size, size_t& len, bool& truncated, const char*& why,
                   Print& log) {
    Lend lend;
    len = 0;
    truncated = false;
    if (!mount(log) || !wakeIfSilent(log)) {
        why = "no card";
        return false;
    }
    FsFile f;
    if (!f.open(name, O_RDONLY)) {
        why = s_sd.exists(name) ? "can't open" : "not found";
        return false;
    }
    int n = f.read(buf, size);
    truncated = n >= 0 && uint64_t(n) < f.fileSize();
    f.close();
    if (n < 0) {
        why = "can't read";
        return false;
    }
    len = size_t(n);
    return true;
}

bool isFolder(const char* name) {
    FsFile f;
    if (!s_sd.fatType() || !f.open(name, O_RDONLY)) return false;
    bool dir = f.isDir();
    f.close();
    return dir;
}

// Device 1's image: its name and how it was opened, to reopen it after the
// card restarts (s_device1_image is declared above, with startCard).
static char s_device1_name[drive_config::MAX_NAME + 1] = "";
static oflag_t s_device1_flags = O_RDONLY;

// A device 1 read, write or sync, through SdFat's file layer, retried once
// after restarting the card if it fails (card_retry::device1Attempt).
template <typename Op>
static bool device1Attempt(Op&& op) {
    return card_retry::device1Attempt(s_watch, s_card_ops, op);
}

// SdFat's file layer, for device 1's image: seek, then read or write.
class Device1ImageFile : public ImageFile {
public:
    uint64_t size() const override { return size_; }
    bool readAt(uint64_t offset, void* data, size_t bytes) override {
        return device1Attempt(
            [&] { return s_device1_image.seekSet(offset) && s_device1_image.read(data, bytes) == int(bytes); });
    }
    bool writeAt(uint64_t offset, const void* data, size_t bytes) override {
        return device1Attempt(
            [&] { return s_device1_image.seekSet(offset) && s_device1_image.write(data, bytes) == bytes; });
    }
    bool sync() override {
        return device1Attempt([] { return s_device1_image.sync(); });
    }
    uint64_t size_ = 0;  // as opened: a reopened file must still be this size
};
static Device1ImageFile s_device1_image_file;

bool SdCardOps::reopenDevice1() {
    return card_retry::reopen(s_device1_image, s_device1_name, s_device1_flags, s_device1_image_file.size_);
}

ImageFile* openDevice1Image(const char* name, Print& log, bool read_only) {
    if (s_device1_image.isOpen()) s_device1_image.close();
    s_device1_name[0] = '\0';
    if (!mount(log) || !wakeIfSilent(log)) {
        s_open_error = "no card";
        return nullptr;
    }
    if (!s_sd.exists(name)) {
        s_open_error = "not found";
        return nullptr;
    }
    if (!s_device1_image.open(name, read_only ? O_RDONLY : O_RDWR)) {
        s_open_error = "can't open";
        log.printf("SD error: can't open %s\n", name);
        return nullptr;
    }
    std::snprintf(s_device1_name, sizeof s_device1_name, "%s", name);
    s_device1_flags = read_only ? O_RDONLY : O_RDWR;
    s_device1_image_file.size_ = s_device1_image.fileSize();
    return &s_device1_image_file;
}

// The folder served for device 1. Entries are found again by their index in
// their folder (FsFile::dirIndex), opening each folder from the served one
// down, so nothing but that index is kept for them.
class SdFolderSource : public folder_disk::FolderSource {
public:
    void serve(const char* name) {
        name_ = name;
        close();
    }

    bool list(uint16_t folder, const folder_disk::Entry* entries, Adder& add) override {
        FsFile dir;
        if (!openEntry(folder, entries, dir) || !dir.isDir()) return false;
        FsFile f;
        char name[256];
        while (f.openNext(&dir, O_RDONLY)) {
            folder_disk::Item item{};
            f.getName(name, sizeof name);
            bool skip = name[0] == '.' || f.isHidden() || f.isSystem();
            if (!skip) {
                item.name = name;
                item.dir = f.isDir();
                item.size = item.dir ? 0 : f.fileSize();
                f.getModifyDateTime(&item.date, &item.time);
                item.locator = f.dirIndex();
            }
            f.close();
            if (!skip && !add.add(item)) break;
        }
        return true;
    }

    bool read(uint16_t entry, const folder_disk::Entry* entries, uint32_t offset, uint8_t* out,
              uint32_t bytes) override {
        if (entry != open_entry_ || !file_.isOpen()) {
            close();
            if (!openEntry(entry, entries, file_)) return false;
            open_entry_ = entry;
        }
        return file_.seekSet(offset) && file_.read(out, bytes) == int(bytes);
    }

private:
    void close() {
        if (file_.isOpen()) file_.close();
        open_entry_ = folder_disk::ROOT;
    }

    // Opens `entry` (ROOT: the folder served) into `out`, by its folders' indexes.
    bool openEntry(uint16_t entry, const folder_disk::Entry* entries, FsFile& out) {
        uint32_t path[16];  // indexes from the served folder down; folders nest less deeply than this
        unsigned depth = 0;
        for (uint16_t e = entry; e != folder_disk::ROOT; e = entries[e].parent) {
            if (depth == 16) return false;
            path[depth++] = entries[e].locator;
        }
        FsFile at;
        if (!at.open(name_, O_RDONLY)) return false;
        while (depth > 0) {
            FsFile next;
            if (!next.open(&at, path[--depth], O_RDONLY)) return false;
            at.close();
            at = next;
        }
        out = at;
        return true;
    }

    const char* name_ = "";
    FsFile file_;
    uint16_t open_entry_ = folder_disk::ROOT;
};
static SdFolderSource s_folder;

folder_disk::FolderSource& folderSource(const char* name) {
    s_folder_served = true;
    s_folder.serve(name);
    return s_folder;
}

}  // namespace sd_card
