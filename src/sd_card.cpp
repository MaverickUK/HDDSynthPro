#include "sd_card.h"

#include <SPI.h>
#include <SdFat.h>

#include "config.h"

namespace sd_card {

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

void printInfo(Print& out) {
    // If a pin were refused, SPI would fall back to its default pins, which
    // on this board are IDE data lines: don't start at all.
    if (!SPI.setSCK(cfg::PIN_SD_CLK) || !SPI.setTX(cfg::PIN_SD_CMD) || !SPI.setRX(cfg::PIN_SD_DAT0)) {
        out.println("SD error: SPI0 refused the SD pins");
        return;
    }
    // 12 MHz to start with: well within what hand-wired leads carry.
    SdSpiConfig config(cfg::PIN_SD_DAT3, DEDICATED_SPI, SD_SCK_MHZ(12), &SPI);
    SdFs sd;
    if (!sd.cardBegin(config)) {
        out.printf("SD error: no card, or it doesn't answer (code %u, data %u)\n",
                   unsigned(sd.sdErrorCode()), unsigned(sd.sdErrorData()));
        sd.end();
        return;
    }
    uint32_t sectors = sd.card()->sectorCount();
    bool mounted = sd.volumeBegin();
    out.printf("SD ok type=%s size_mb=%lu fs=%s\n", cardType(sd.card()->type()),
               (unsigned long)(sectors / 2048), mounted ? fsType(sd.fatType()) : "none");
    sd.end();
}

}  // namespace sd_card
