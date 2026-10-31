#include "sd_card.h"

#include <SPI.h>
#include <SdFat.h>

#include "config.h"
#include "drive_config.h"

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

// Puts SPI0 on the SD pins.
static bool spiPins(Print& out) {
    // If a pin were refused, SPI would fall back to its default pins, which
    // on this board are IDE data lines: don't start at all.
    if (!SPI.setSCK(cfg::PIN_SD_CLK) || !SPI.setTX(cfg::PIN_SD_CMD) || !SPI.setRX(cfg::PIN_SD_DAT0)) {
        out.println("SD error: SPI0 refused the SD pins");
        return false;
    }
    return true;
}

// Starts the card and mounts its volume (a fresh volume: whatever SdFat had
// cached from a card before is dropped).
static bool startCard() {
    SdSpiConfig config(cfg::PIN_SD_DAT3, DEDICATED_SPI, SD_SCK_MHZ(cfg::SD_SPI_MHZ), &SPI);
    return s_sd.begin(config);
}

// Mounts the card's volume, unless it already is.
static bool mount(Print& out) {
    if (s_sd.fatType()) return true;
    if (!spiPins(out)) return false;
    if (!startCard()) {
        out.printf("SD error: no card, or no FAT volume on it (code %u, data %u)\n",
                   unsigned(s_sd.sdErrorCode()), unsigned(s_sd.sdErrorData()));
        return false;
    }
    return true;
}

// "open" (being served), "present" or "none".
static const char* imageState(const char* name) {
    if (s_image.isOpen() && s_open_name && strcasecmp(s_open_name, name) == 0) return "open";
    return s_sd.exists(name) ? "present" : "none";
}

void printInfo(Print& out) {
    if (!mount(out)) return;
    uint32_t sectors = s_sd.card()->sectorCount();
    out.printf("SD ok type=%s size_mb=%lu fs=%s image=%s bench=%s config=%s\n", cardType(s_sd.card()->type()),
               (unsigned long)(sectors / 2048), fsType(s_sd.fatType()),
               imageState(IMAGE_NAME), imageState(BENCH_IMAGE_NAME),
               s_sd.exists(drive_config::FILE_NAME) ? "present" : "none");
}

// ---- Watching the card (media_watch.h) ---------------------------------------

static media_watch::Watch s_watch;
static const char* s_served_name = nullptr;  // the image being served, kept while the card is out
static uint64_t s_served_size = 0;           // its size, which the drive told the host

// Whether a card answers CMD0 (go idle, and into SPI mode), the first step of
// starting one. Sent by hand at 400 kHz, because SdFat's start-up retries it
// for 2 s when there's no card; this takes about half a millisecond. With no
// card the pull-up on DAT0 reads 0xFF. Only while the card is lost: CMD0
// resets a card, so SdFat must start it again afterwards.
static bool cardAnswers() {
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
    id.size = s_image.fileSize();
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
    if (!startCard()) {
        s_watch.cameBackWithout("can't mount", now);
        return false;
    }
    if (!s_image.open(s_served_name, O_RDWR)) {
        s_watch.cameBackWithout(s_sd.exists(s_served_name) ? "can't open" : "image not found", now);
        return false;
    }
    media_watch::Identity id;
    if (!identify(id)) {
        s_watch.cameBackWithout("can't read the card's ID", now);
    } else if (s_watch.cameBack(id, now) == media_watch::State::Present) {
        checkContiguous();
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

const media_watch::Watch& media() { return s_watch; }

void serviceMedia() {
    uint32_t now = millis();
    if (!s_watch.due(now)) return;
    if (s_watch.state() == media_watch::State::Lost) {
        reattach(now);
    } else {
        cid_t cid;  // a different card: still in?
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

// A FAT32 chain, from the image's first cluster, into s_extents. False if it
// can't be (then s_extent_count is 0).
static bool walkFat32(uint32_t image_sectors) {
    const uint32_t spc = s_sd.bytesPerCluster() / 512;
    const Sector_t data = s_sd.dataStartSector(), fat = s_sd.fatStartSector();
    if (spc == 0 || s_image.firstSector() < data) return false;
    uint32_t cluster = uint32_t((s_image.firstSector() - data) / spc) + 2;
    static uint8_t sector[512];
    Sector_t cached = ~Sector_t(0);
    uint32_t file_sector = 0;
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

static bool mapAgrees(uint32_t image_sectors);  // below

// Maps the open image (s_extents), and tells SdFat if it's one run, so its own
// seeks are sums too.
static void checkContiguous() {
    s_extent_count = 0;
    s_image.sync();  // nothing of the image left in SdFat's cache: from here on it's written directly
    uint32_t image_sectors = uint32_t((s_image.fileSize() + 511) / 512);
    Sector_t first = 0, last = 0;
    s_contiguous = s_image.contiguousRange(&first, &last);
    if (s_contiguous) {
        s_extents[0] = {0, uint32_t(first), uint32_t(last - first + 1)};
        s_extent_count = 1;
    } else if (s_sd.fatType() != FAT_TYPE_FAT32 || !walkFat32(image_sectors)) {
        s_extent_count = 0;
    }
    if (s_extent_count && !mapAgrees(image_sectors)) s_extent_count = 0;
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
static bool mapAgrees(uint32_t image_sectors) {
    static uint8_t a[512], b[512];
    uint32_t samples[8];
    size_t n = 0;
    for (size_t e = 0; e < s_extent_count && n < 4; ++e) samples[n++] = s_extents[e].file_sector;
    samples[n++] = s_extents[s_extent_count - 1].file_sector;
    samples[n++] = image_sectors / 2;
    samples[n++] = image_sectors - 1;
    for (size_t i = 0; i < n; ++i) {
        uint32_t lba = samples[i];
        bool ok = mapped(uint64_t(lba) * 512, 512, [&](Sector_t at, uint32_t, uint32_t count) {
            return s_sd.card()->readSectors(at, a, count);
        });
        if (!ok || !s_image.seekSet(uint64_t(lba) * 512) || s_image.read(b, 512) != 512 || memcmp(a, b, 512) != 0)
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

ImageFile* openImage(const char* name, Print& log) {
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
    if (!s_image.open(name, O_RDWR)) {
        s_open_error = "can't open";
        log.printf("SD error: can't open %s\n", name);
        return nullptr;
    }
    media_watch::Identity id;
    if (!identify(id)) {
        s_image.close();
        s_open_error = "can't open";
        log.printf("SD error: can't read the card's ID\n");
        return nullptr;
    }
    checkContiguous();
    s_open_name = name;
    s_served_name = name;
    s_served_size = id.size;
    s_watch.serve(id);
    return &s_image_file;
}

const char* openError() { return s_open_error; }

bool imageContiguous() { return s_contiguous; }

unsigned imageExtents() { return unsigned(s_extent_count); }

bool configExists() { return s_sd.fatType() && s_sd.exists(drive_config::FILE_NAME); }

bool mounted() { return s_sd.fatType() != 0; }

bool readConfig(char* buf, size_t size, size_t& len, bool& truncated, Print& log) {
    len = 0;
    truncated = false;
    if (!mount(log)) return false;
    FsFile f;
    if (!f.open(drive_config::FILE_NAME, O_RDONLY)) return false;
    int n = f.read(buf, size);
    truncated = n >= 0 && uint64_t(n) < f.fileSize();
    f.close();
    if (n < 0) return false;
    len = size_t(n);
    return true;
}

bool writeConfig(const char* text, size_t len, Print& out) {
    if (!mount(out)) return false;
    FsFile f;
    if (!f.open(drive_config::FILE_NAME, O_RDWR | O_CREAT | O_TRUNC)) {
        out.printf("SD error: can't write %s\n", drive_config::FILE_NAME);
        return false;
    }
    bool ok = f.write(text, len) == len && f.sync();
    f.close();
    if (!ok) out.printf("SD error: writing %s failed (code %u)\n", drive_config::FILE_NAME, unsigned(s_sd.sdErrorCode()));
    return ok;
}

bool createImage(const char* name, uint32_t sectors, Print& out) {
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

}  // namespace sd_card
