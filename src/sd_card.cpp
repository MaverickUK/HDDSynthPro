#include "sd_card.h"

#include <SPI.h>
#include <SdFat.h>

#include "config.h"

namespace sd_card {

static SdFs s_sd;
static FsFile s_image;

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
    // 12 MHz: well within what hand-wired leads carry.
    SdSpiConfig config(cfg::PIN_SD_DAT3, DEDICATED_SPI, SD_SCK_MHZ(12), &SPI);
    if (!s_sd.begin(config)) {
        out.printf("SD error: no card, or no FAT volume on it (code %u, data %u)\n",
                   unsigned(s_sd.sdErrorCode()), unsigned(s_sd.sdErrorData()));
        return false;
    }
    return true;
}

void printInfo(Print& out) {
    if (!mount(out)) return;
    uint32_t sectors = s_sd.card()->sectorCount();
    out.printf("SD ok type=%s size_mb=%lu fs=%s image=%s\n", cardType(s_sd.card()->type()),
               (unsigned long)(sectors / 2048), fsType(s_sd.fatType()),
               s_image.isOpen() ? "open" : (s_sd.exists(IMAGE_NAME) ? "present" : "none"));
}

// HDDSYNTH.IMG as the drive's ImageFile.
class SdImageFile : public ImageFile {
public:
    uint64_t size() const override { return s_image.fileSize(); }
    bool readAt(uint64_t offset, void* data, size_t bytes) override {
        return s_image.seekSet(offset) && s_image.read(data, bytes) == int(bytes);
    }
    bool writeAt(uint64_t offset, const void* data, size_t bytes) override {
        return s_image.seekSet(offset) && s_image.write(data, bytes) == bytes;
    }
};
static SdImageFile s_image_file;

ImageFile* openImage(Print& log) {
    if (!mount(log)) return nullptr;
    if (!s_sd.exists(IMAGE_NAME)) return nullptr;
    if (!s_image.open(IMAGE_NAME, O_RDWR)) {
        log.printf("SD error: can't open %s\n", IMAGE_NAME);
        return nullptr;
    }
    return &s_image_file;
}

bool createImage(uint32_t sectors, Print& out) {
    if (!mount(out)) return false;
    if (s_sd.exists(IMAGE_NAME)) {
        out.printf("# %s already exists: left as it is\n", IMAGE_NAME);
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
    if (!file.open(IMAGE_NAME, O_RDWR | O_CREAT | O_EXCL)) {
        out.printf("SD error: can't create %s\n", IMAGE_NAME);
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
        s_sd.remove(IMAGE_NAME);  // our own half-made file, nothing else
        out.printf("SD error: writing %s failed (code %u); removed it\n", IMAGE_NAME,
                   unsigned(s_sd.sdErrorCode()));
        return false;
    }
    out.printf("# created %s, %lu sectors; reboot the drive to use it\n", IMAGE_NAME,
               (unsigned long)sectors);
    return true;
}

}  // namespace sd_card
