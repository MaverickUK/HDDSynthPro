#include "sd_card.h"

#include <SPI.h>
#include <SdFat.h>

#include "config.h"

namespace sd_card {

static SdFs s_sd;
static FsFile s_image;
static const char* s_open_name = nullptr;  // which image s_image is

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

// Mounts the card's volume, unless it already is.
static bool mount(Print& out) {
    if (s_sd.fatType()) return true;
    if (!spiPins(out)) return false;
    SdSpiConfig config(cfg::PIN_SD_DAT3, DEDICATED_SPI, SD_SCK_MHZ(cfg::SD_SPI_MHZ), &SPI);
    if (!s_sd.begin(config)) {
        out.printf("SD error: no card, or no FAT volume on it (code %u, data %u)\n",
                   unsigned(s_sd.sdErrorCode()), unsigned(s_sd.sdErrorData()));
        return false;
    }
    return true;
}

// "open" (being served), "present" or "none".
static const char* imageState(const char* name) {
    if (s_image.isOpen() && s_open_name == name) return "open";
    return s_sd.exists(name) ? "present" : "none";
}

void printInfo(Print& out) {
    if (!mount(out)) return;
    uint32_t sectors = s_sd.card()->sectorCount();
    out.printf("SD ok type=%s size_mb=%lu fs=%s image=%s bench=%s\n", cardType(s_sd.card()->type()),
               (unsigned long)(sectors / 2048), fsType(s_sd.fatType()),
               imageState(IMAGE_NAME), imageState(BENCH_IMAGE_NAME));
}

// The open image as the drive's ImageFile.
class SdImageFile : public ImageFile {
public:
    uint64_t size() const override { return s_image.fileSize(); }
    bool readAt(uint64_t offset, void* data, size_t bytes) override {
        return s_image.seekSet(offset) && s_image.read(data, bytes) == int(bytes);
    }
    bool writeAt(uint64_t offset, const void* data, size_t bytes) override {
        return s_image.seekSet(offset) && s_image.write(data, bytes) == bytes;
    }
    // SdFat's cache to the card, then waits until the card isn't busy.
    bool sync() override { return s_image.sync(); }
};
static SdImageFile s_image_file;

ImageFile* openImage(const char* name, Print& log) {
    if (!mount(log)) return nullptr;
    if (!s_sd.exists(name)) return nullptr;
    if (!s_image.open(name, O_RDWR)) {
        log.printf("SD error: can't open %s\n", name);
        return nullptr;
    }
    s_open_name = name;
    return &s_image_file;
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
