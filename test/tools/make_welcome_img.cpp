// Writes the welcome disk (welcome_disk.h) to a file, to see README.TXT as
// a computer will before flashing the drive:
//
//   .pio/native-tests/make_welcome_img welcome.img
//   open welcome.img          (macOS mounts it; Linux: sudo mount -o loop,offset=32256)
//
// The same files as the firmware serves: README.TXT from welcome/README.TXT
// and HDDSYNTH.INI with the settings' defaults.
#include <cstdio>
#include <cstring>

#include "drive_config.h"
#include "welcome_disk.h"
#include "welcome_text.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output file>\n", argv[0]);
        return 2;
    }
    const welcome_disk::File files[] = {
        {"README.TXT", welcome_text::README, welcome_text::README_SIZE},
        {drive_config::FILE_NAME, drive_config::defaultFile(), uint32_t(std::strlen(drive_config::defaultFile()))},
    };
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) {
        std::perror(argv[1]);
        return 1;
    }
    uint8_t sector[welcome_disk::SECTOR_BYTES];
    for (uint32_t lba = 0; lba < welcome_disk::SECTORS; ++lba) {
        welcome_disk::sector(lba, files, 2, welcome_text::FAT_DATE, sector);
        if (std::fwrite(sector, sizeof sector, 1, f) != 1) {
            std::perror(argv[1]);
            return 1;
        }
    }
    return std::fclose(f) == 0 ? 0 : 1;
}
